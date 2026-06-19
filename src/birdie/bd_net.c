/*
 * bd_net -- birdie's network connection layer.
 *
 * A single TCP/telnet connection to a MUD. The socket lives on a dedicated
 * network thread (a libiox poll loop); resolution, connect, recv/send, and
 * telnet option negotiation (bd_telopt) all happen there so the UI thread
 * never blocks on the network. Two lock-free SPSC rings carry framed messages:
 *
 *   tx (UI -> net):  CONNECT / SEND / CLOSE / WINSIZE / TERMTYPE
 *   rx (net -> UI):  DATA / STATE / PROMPT / ECHO
 *
 * The UI wakes the net thread with a self-pipe byte; bd_net_poll() drains rx
 * and fires the callbacks, all on the UI thread.
 *
 * Inbound bytes go through bd_telopt, whose output (clean data, plus prompt
 * and echo events) is framed into a net-thread staging buffer and streamed
 * into the rx ring. Streaming, rather than one ring_put per record, keeps
 * backpressure exact even when one recv yields several records: the ring's
 * framed reader already waits for a record to arrive in full.
 *
 * Deferred (doc/network.md): TLS (mbedTLS), the MTH extension set
 * (GMCP/MSDP/MSSP/MCCP), Happy Eyeballs, reconnect, and a Winsock path.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#include "bd_net.h"
#include "bd_ring.h"
#include "bd_telopt.h"
#include "iox_loop.h"
#include "iox_fd.h"
#include "iox_timer.h"

#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <miniz.h>

#include <psa/crypto.h>
#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/error.h>

#define TN_IAC  255

/* tx message types (UI -> net) */
enum {
	MSG_CONNECT,    /* host '\0' port '\0' */
	MSG_SEND,       /* bytes to transmit */
	MSG_CLOSE,
	MSG_WINSIZE,    /* u16 cols, u16 rows (native order) */
	MSG_TERMTYPE    /* terminal type string */
};

/* rx message types (net -> UI) */
enum {
	MSG_DATA,       /* clean received bytes */
	MSG_STATE,      /* [u8 state][detail string] */
	MSG_PROMPT,     /* no payload */
	MSG_ECHO,       /* [u8 suppress] */
	MSG_PKG         /* [u8 proto][name '\0'][json] -- GMCP/MSDP package */
};

#define RX_CAP   (1u << 20)     /* 1 MiB inbound buffer */
#define TX_CAP   (1u << 16)     /* 64 KiB command buffer */
#define CHUNK    4096

#define RECONNECT_BASE_MS  1000     /* first auto-reconnect delay */
#define RECONNECT_MAX_MS  60000     /* exponential backoff ceiling */

struct bd_net {
	bd_net_data_cb on_data;
	bd_net_state_cb on_state;
	bd_net_echo_cb on_echo;
	bd_net_prompt_cb on_prompt;
	bd_net_package_cb on_package;
	void *arg;

	_Atomic int state;
	_Atomic int shutdown;
	_Atomic int autoreconnect;      /* 1 = auto-reconnect on unexpected drop */

	bd_ring *rx;            /* net -> UI */
	bd_ring *tx;            /* UI -> net */
	int wake_r, wake_w;

	pthread_t thread;
	int have_thread;
};

/* Net-thread-only working state. */
struct net_ctx {
	bd_net *n;
	struct iox_loop *loop;
	bd_telopt *telopt;
	int fd;
	int connecting;
	unsigned want;
	int rx_blocked;         /* rx ring full: reads paused */
	int quit;

	unsigned char *out;     /* bytes owed to the socket */
	size_t out_len, out_cap, out_sent;

	unsigned char *pend;    /* framed rx records not yet in the ring */
	size_t pend_len, pend_cap, pend_sent;

	/* MCCP2 inbound decompression */
	int want_inflate;       /* telopt signalled the compressed stream begins */
	int inflating;          /* recv bytes are a zlib stream */
	int zs_inited;
	mz_stream zs;
	unsigned char cin[CHUNK];  /* compressed bytes read but not yet inflated */
	size_t cin_len, cin_pos;

	/* TLS (per connection) */
	/* reconnect target + backoff */
	int have_target;        /* host/port/tls below are valid */
	int user_closed;        /* last close was user-initiated: no reconnect */
	char port[64];
	int backoff_ms;         /* next auto-reconnect delay */
	int reconnect_timer;    /* pending reconnect timer id, or -1 */

	int tls;                /* this connection is TLS */
	int handshaking;        /* TLS handshake in progress */
	unsigned hs_want;       /* iox interest the handshake is blocked on */
	char host[256];         /* target host; for SNI / cert verification */
	mbedtls_ssl_context ssl;
	int ssl_inited;

	/* TLS (shared across connections, set up once on the thread) */
	int tls_ready;
	int tls_insecure;       /* BIRDIE_TLS_INSECURE: skip cert verification */
	mbedtls_ssl_config conf;
	mbedtls_ctr_drbg_context drbg;
	mbedtls_entropy_context entropy;
	mbedtls_x509_crt cacert;
};

/* ---- framed-message helpers over a ring -------------------------------- */

static int
ring_put(bd_ring *r, uint8_t type, const void *payload, uint32_t len)
{
	unsigned char hdr[5];
	hdr[0] = type;
	memcpy(hdr + 1, &len, sizeof len);
	return bd_ring_writev(r, hdr, sizeof hdr, payload, len);
}

static int
ring_get(bd_ring *r, uint8_t *type, unsigned char *buf, uint32_t cap,
         uint32_t *len)
{
	unsigned char hdr[5];
	uint32_t l, n;

	if (bd_ring_read_avail(r) < sizeof hdr)
		return 0;
	bd_ring_peek(r, hdr, sizeof hdr);
	memcpy(&l, hdr + 1, sizeof l);
	if (bd_ring_read_avail(r) < sizeof hdr + (size_t)l)
		return 0;

	bd_ring_skip(r, sizeof hdr);
	*type = hdr[0];
	n = l < cap ? l : cap;
	bd_ring_read(r, buf, n);
	if (l > n)
		bd_ring_skip(r, l - n);
	*len = n;
	return 1;
}

static void
wake(bd_net *n)
{
	unsigned char b = 1;
	ssize_t rc = write(n->wake_w, &b, 1);
	(void)rc;
}

/* ---- net thread: staging buffer -> rx ring ---------------------------- */

/* Append a framed record to the pending staging buffer. */
static void
pend_put(struct net_ctx *c, uint8_t type, const void *payload, uint32_t len)
{
	size_t need = c->pend_len + 5 + len;

	if (c->pend_sent == c->pend_len)
		c->pend_sent = c->pend_len = 0;
	if (need > c->pend_cap) {
		size_t cap = c->pend_cap ? c->pend_cap * 2 : 8192;
		unsigned char *nb;
		while (cap < need)
			cap *= 2;
		nb = realloc(c->pend, cap);
		if (!nb)
			return;         /* drop on OOM */
		c->pend = nb;
		c->pend_cap = cap;
	}
	c->pend[c->pend_len++] = type;
	memcpy(c->pend + c->pend_len, &len, 4);
	c->pend_len += 4;
	if (len) {
		memcpy(c->pend + c->pend_len, payload, len);
		c->pend_len += len;
	}
}

/* Stream as many staged bytes into the rx ring as fit. Partial records are
 * fine: the consumer waits until a record is complete. */
static void
drain_pend(struct net_ctx *c)
{
	size_t avail = bd_ring_write_avail(c->n->rx);
	size_t rem = c->pend_len - c->pend_sent;
	size_t k = rem < avail ? rem : avail;

	if (k) {
		bd_ring_write(c->n->rx, c->pend + c->pend_sent, k);
		c->pend_sent += k;
	}
	if (c->pend_sent == c->pend_len) {
		c->pend_sent = c->pend_len = 0;
		c->rx_blocked = 0;
	} else {
		c->rx_blocked = 1;
	}
}

static void
push_state(struct net_ctx *c, bd_net_state st, const char *msg)
{
	unsigned char frame[1 + 200];
	uint32_t len = 1;
	frame[0] = (unsigned char)st;
	if (msg) {
		size_t m = strlen(msg);
		if (m > sizeof frame - 1)
			m = sizeof frame - 1;
		memcpy(frame + 1, msg, m);
		len += (uint32_t)m;
	}
	atomic_store(&c->n->state, (int)st);
	if (st == BD_NET_CONNECTED)
		c->backoff_ms = RECONNECT_BASE_MS;      /* success resets backoff */
	pend_put(c, MSG_STATE, frame, len);
	drain_pend(c);
}

/* ---- net thread: socket out buffer ------------------------------------ */

static int
out_push(struct net_ctx *c, const unsigned char *p, size_t len)
{
	if (c->out_sent == c->out_len)
		c->out_sent = c->out_len = 0;
	if (c->out_len + len > c->out_cap) {
		size_t cap = c->out_cap ? c->out_cap * 2 : 256;
		unsigned char *nb;
		while (cap < c->out_len + len)
			cap *= 2;
		nb = realloc(c->out, cap);
		if (!nb)
			return -1;
		c->out = nb;
		c->out_cap = cap;
	}
	memcpy(c->out + c->out_len, p, len);
	c->out_len += len;
	return 0;
}

/* ---- bd_telopt callbacks (run on the net thread) ---------------------- */

static void
telopt_data(const unsigned char *p, size_t len, void *arg)
{
	struct net_ctx *c = arg;
	pend_put(c, MSG_DATA, p, (uint32_t)len);
}

static void
telopt_xmit(const unsigned char *p, size_t len, void *arg)
{
	struct net_ctx *c = arg;
	out_push(c, p, len);
}

static void
telopt_prompt(void *arg)
{
	struct net_ctx *c = arg;
	pend_put(c, MSG_PROMPT, NULL, 0);
}

static void
telopt_echo(int suppress, void *arg)
{
	struct net_ctx *c = arg;
	unsigned char b = suppress ? 1 : 0;
	pend_put(c, MSG_ECHO, &b, 1);
}

/* Frame a package as [u8 proto][name '\0'][json] into a stack buffer (or a
 * heap one if large) and stage it for the UI. */
static void
telopt_package(int proto, const char *name, const char *json, void *arg)
{
	struct net_ctx *c = arg;
	size_t nl = strlen(name), jl = strlen(json);
	size_t total = 1 + nl + 1 + jl;
	unsigned char stackbuf[1024];
	unsigned char *buf = stackbuf;

	if (total > sizeof stackbuf) {
		buf = malloc(total);
		if (!buf)
			return;
	}
	buf[0] = (unsigned char)proto;
	memcpy(buf + 1, name, nl + 1);          /* includes NUL */
	memcpy(buf + 1 + nl + 1, json, jl);
	pend_put(c, MSG_PKG, buf, (uint32_t)total);
	if (buf != stackbuf)
		free(buf);
}

static void
telopt_compress(void *arg)
{
	struct net_ctx *c = arg;
	c->want_inflate = 1;            /* do_read switches to inflate next */
}

/* ---- net thread: I/O -------------------------------------------------- */

static int
set_nonblock(int fd)
{
	int fl = fcntl(fd, F_GETFL, 0);
	if (fl < 0)
		return -1;
	return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static void
update_interest(struct net_ctx *c)
{
	unsigned ev = 0;

	if (c->fd < 0)
		return;
	if (c->connecting) {
		ev = IOX_WRITE;
	} else if (c->handshaking) {
		ev = c->hs_want;        /* whatever the TLS handshake is waiting on */
	} else {
		if (!c->rx_blocked)
			ev |= IOX_READ;
		if (c->out_sent < c->out_len)
			ev |= IOX_WRITE;
	}
	if (ev != c->want) {
		iox_fd_mod(c->loop, c->fd, ev);
		c->want = ev;
	}
}

static void
close_socket(struct net_ctx *c)
{
	if (c->ssl_inited) {
		mbedtls_ssl_free(&c->ssl);
		c->ssl_inited = 0;
	}
	if (c->zs_inited) {
		mz_inflateEnd(&c->zs);
		c->zs_inited = 0;
	}
	if (c->fd >= 0) {
		iox_fd_remove(c->loop, c->fd);
		close(c->fd);
		c->fd = -1;
	}
	c->connecting = 0;
	c->handshaking = 0;
	c->want = 0;
	c->hs_want = 0;
	c->rx_blocked = 0;
	c->out_len = c->out_sent = 0;
	c->want_inflate = c->inflating = 0;
	c->cin_len = c->cin_pos = 0;
}

static void sock_cb(struct iox_loop *loop, int fd, unsigned events, void *arg);
static void connect_now(struct net_ctx *c);
static void reconnect_cb(struct iox_loop *loop, void *arg);

static void
cancel_reconnect(struct net_ctx *c)
{
	if (c->reconnect_timer >= 0) {
		iox_timer_remove(c->loop, c->reconnect_timer);
		c->reconnect_timer = -1;
	}
}

static int
should_reconnect(struct net_ctx *c)
{
	return !c->user_closed && c->have_target && c->reconnect_timer < 0 &&
	    !atomic_load(&c->n->shutdown) && atomic_load(&c->n->autoreconnect);
}

/* Tear down after an unexpected disconnect or failed (re)connect, report it,
 * and arm an exponential-backoff reconnect unless it was user-initiated or
 * auto-reconnect is off. */
static void
disconnect(struct net_ctx *c, bd_net_state st, const char *base)
{
	char msg[256];
	int recon, delay;

	close_socket(c);
	recon = should_reconnect(c);
	delay = c->backoff_ms;
	if (!base)
		base = "disconnected";
	if (recon)
		snprintf(msg, sizeof msg, "%s; reconnecting in %ds",
		    base, (delay + 999) / 1000);
	else
		snprintf(msg, sizeof msg, "%s", base);
	push_state(c, st, msg);

	if (recon) {
		c->reconnect_timer = iox_timer_add(c->loop, delay,
		    reconnect_cb, c);
		c->backoff_ms = c->backoff_ms > RECONNECT_MAX_MS / 2
		    ? RECONNECT_MAX_MS : c->backoff_ms * 2;
	}
}

static void
reconnect_cb(struct iox_loop *loop, void *arg)
{
	struct net_ctx *c = arg;
	(void)loop;
	c->reconnect_timer = -1;
	connect_now(c);
}

/* ---- net thread: TLS -------------------------------------------------- */

/* mbedTLS BIO over the non-blocking socket. EAGAIN maps to WANT_READ/WRITE so
 * the handshake and reads/writes resume when the poll loop fires again. */
static int
ssl_bio_send(void *ctx, const unsigned char *buf, size_t len)
{
	struct net_ctx *c = ctx;
	ssize_t w = send(c->fd, buf, len, 0);
	if (w >= 0)
		return (int)w;
	if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
		return MBEDTLS_ERR_SSL_WANT_WRITE;
	return MBEDTLS_ERR_NET_SEND_FAILED;
}

static int
ssl_bio_recv(void *ctx, unsigned char *buf, size_t len)
{
	struct net_ctx *c = ctx;
	ssize_t r = recv(c->fd, buf, len, 0);
	if (r > 0)
		return (int)r;
	if (r == 0)
		return 0;               /* EOF: mbedtls surfaces it to ssl_read */
	if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
		return MBEDTLS_ERR_SSL_WANT_READ;
	return MBEDTLS_ERR_NET_RECV_FAILED;
}

/* Drive the TLS handshake one step. On WANT_READ/WRITE it parks until the next
 * socket event; on success the connection becomes ready; otherwise it errors. */
static void
do_handshake(struct net_ctx *c)
{
	int rc = mbedtls_ssl_handshake(&c->ssl);

	if (rc == MBEDTLS_ERR_SSL_WANT_READ) {
		c->hs_want = IOX_READ;
		update_interest(c);
		return;
	}
	if (rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
		c->hs_want = IOX_WRITE;
		update_interest(c);
		return;
	}
	if (rc != 0) {
		char msg[96];
		mbedtls_strerror(rc, msg, sizeof msg);
		disconnect(c, BD_NET_ERROR, msg);
		return;
	}
	/* handshake complete */
	c->handshaking = 0;
	c->hs_want = 0;
	update_interest(c);             /* back to READ + pending writes */
	push_state(c, BD_NET_CONNECTED, NULL);
}

/* Begin TLS on a freshly connected socket. */
static void
start_tls(struct net_ctx *c)
{
	int rc;

	if (!c->tls_ready) {
		disconnect(c, BD_NET_ERROR, "TLS unavailable");
		return;
	}
	mbedtls_ssl_init(&c->ssl);
	c->ssl_inited = 1;
	rc = mbedtls_ssl_setup(&c->ssl, &c->conf);
	if (rc == 0)
		rc = mbedtls_ssl_set_hostname(&c->ssl, c->host);
	if (rc != 0) {
		char msg[96];
		mbedtls_strerror(rc, msg, sizeof msg);
		disconnect(c, BD_NET_ERROR, msg);
		return;
	}
	mbedtls_ssl_set_bio(&c->ssl, c, ssl_bio_send, ssl_bio_recv, NULL);
	c->handshaking = 1;
	do_handshake(c);
}

/* Dial the stored target. Used for the initial connect and each reconnect. */
static void
connect_now(struct net_ctx *c)
{
	struct addrinfo hints, *res, *ai;
	int rc;

	close_socket(c);
	bd_telopt_reset(c->telopt);
	c->pend_len = c->pend_sent = 0;

	push_state(c, BD_NET_CONNECTING, NULL);

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	rc = getaddrinfo(c->host, c->port, &hints, &res); /* blocking; net thread */
	if (rc != 0) {
		disconnect(c, BD_NET_ERROR, gai_strerror(rc));
		return;
	}

	for (ai = res; ai; ai = ai->ai_next) {
		int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (fd < 0)
			continue;
		if (set_nonblock(fd) < 0) {
			close(fd);
			continue;
		}
		rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
		if (rc == 0) {
			c->fd = fd;
			c->connecting = 0;
			iox_fd_add(c->loop, fd, IOX_READ, sock_cb, c);
			c->want = IOX_READ;
			freeaddrinfo(res);
			if (c->tls)
				start_tls(c);
			else
				push_state(c, BD_NET_CONNECTED, NULL);
			return;
		}
		if (rc < 0 && errno == EINPROGRESS) {
			c->fd = fd;
			c->connecting = 1;
			iox_fd_add(c->loop, fd, IOX_WRITE, sock_cb, c);
			c->want = IOX_WRITE;
			freeaddrinfo(res);
			return;
		}
		close(fd);
	}
	freeaddrinfo(res);
	disconnect(c, BD_NET_ERROR, "could not connect");
}

/* Handle a user CONNECT command: record the target, then dial. */
static void
do_connect(struct net_ctx *c, const unsigned char *payload, uint32_t len)
{
	const char *host, *port;
	size_t hl, pl;

	/* payload: [u8 tls] host '\0' port '\0' */
	if (len < 1) {
		push_state(c, BD_NET_ERROR, "bad connect target");
		return;
	}
	host = (const char *)payload + 1;
	hl = strnlen(host, len - 1);
	if (hl >= len - 1) {
		push_state(c, BD_NET_ERROR, "bad connect target");
		return;
	}
	port = host + hl + 1;
	pl = strnlen(port, len - 1 - hl - 1);

	c->tls = payload[0] ? 1 : 0;
	memcpy(c->host, host, hl < sizeof c->host ? hl : sizeof c->host - 1);
	c->host[hl < sizeof c->host ? hl : sizeof c->host - 1] = '\0';
	memcpy(c->port, port, pl < sizeof c->port ? pl : sizeof c->port - 1);
	c->port[pl < sizeof c->port ? pl : sizeof c->port - 1] = '\0';
	c->have_target = 1;
	c->user_closed = 0;

	cancel_reconnect(c);
	c->backoff_ms = RECONNECT_BASE_MS;
	connect_now(c);
}

static void
do_send(struct net_ctx *c, const unsigned char *p, uint32_t len)
{
	uint32_t i, start;

	if (c->fd < 0 || c->connecting)
		return;

	/* escape 0xFF -> IAC IAC so payload is not read as telnet commands */
	for (i = 0, start = 0; i < len; i++) {
		if (p[i] == TN_IAC) {
			unsigned char esc[2] = { TN_IAC, TN_IAC };
			if (i > start)
				out_push(c, p + start, i - start);
			out_push(c, esc, 2);
			start = i + 1;
		}
	}
	if (len > start)
		out_push(c, p + start, len - start);
	update_interest(c);
}

/* Transport read/write over plain TCP or TLS. Return >0 bytes moved, 0 on
 * orderly peer close, -1 if the operation would block (retry on next event),
 * or -2 on a fatal error (emsg filled). */
static ssize_t
transport_read(struct net_ctx *c, unsigned char *buf, size_t len,
               char *emsg, size_t ecap)
{
	if (c->tls) {
		int r = mbedtls_ssl_read(&c->ssl, buf, len);
		if (r > 0)
			return r;
		if (r == MBEDTLS_ERR_SSL_WANT_READ ||
		    r == MBEDTLS_ERR_SSL_WANT_WRITE)
			return -1;
		if (r == 0 || r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
			return 0;
		mbedtls_strerror(r, emsg, ecap);
		return -2;
	} else {
		ssize_t r = recv(c->fd, buf, len, 0);
		if (r > 0)
			return r;
		if (r == 0)
			return 0;
		if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
			return -1;
		snprintf(emsg, ecap, "%s", strerror(errno));
		return -2;
	}
}

static int
transport_write(struct net_ctx *c, const unsigned char *buf, size_t len,
                char *emsg, size_t ecap)
{
	if (c->tls) {
		int w = mbedtls_ssl_write(&c->ssl, buf, len);
		if (w > 0)
			return w;
		if (w == MBEDTLS_ERR_SSL_WANT_READ ||
		    w == MBEDTLS_ERR_SSL_WANT_WRITE)
			return -1;
		mbedtls_strerror(w, emsg, ecap);
		return -2;
	} else {
		ssize_t w = send(c->fd, buf, len, 0);
		if (w > 0)
			return (int)w;
		if (w == 0 || errno == EAGAIN || errno == EWOULDBLOCK ||
		    errno == EINTR)
			return -1;
		snprintf(emsg, ecap, "%s", strerror(errno));
		return -2;
	}
}

static void
flush_out(struct net_ctx *c)
{
	while (c->out_sent < c->out_len) {
		char emsg[96];
		int w = transport_write(c, c->out + c->out_sent,
		    c->out_len - c->out_sent, emsg, sizeof emsg);
		if (w > 0) {
			c->out_sent += (size_t)w;
			continue;
		}
		if (w == -1)
			return;                 /* retry on next event */
		disconnect(c, BD_NET_ERROR, emsg);
		return;
	}
	if (c->out_sent == c->out_len)
		c->out_sent = c->out_len = 0;
}

/* Begin MCCP2: bytes following the start signal are a zlib stream. */
static int
start_inflate(struct net_ctx *c)
{
	memset(&c->zs, 0, sizeof c->zs);
	if (mz_inflateInit(&c->zs) != MZ_OK)
		return -1;
	c->zs_inited = 1;
	c->inflating = 1;
	c->want_inflate = 0;
	return 0;
}

/* Pull bytes from the connection (via TLS or plain), inflating them first if
 * MCCP2 is active, and feed the result through telopt into the rx ring. Loops
 * until the socket would block or backpressure pauses us. */
static void
do_read(struct net_ctx *c)
{
	for (;;) {
		char emsg[96];

		/* flush staged records before producing more */
		if (c->pend_sent < c->pend_len) {
			drain_pend(c);
			if (c->pend_sent < c->pend_len)
				return;
		}
		if (bd_ring_write_avail(c->n->rx) == 0) {
			c->rx_blocked = 1;
			return;
		}

		if (c->inflating) {
			unsigned char scratch[CHUNK];
			size_t produced;
			int zr;

			if (c->cin_pos >= c->cin_len) {
				ssize_t r = transport_read(c, c->cin,
				    sizeof c->cin, emsg, sizeof emsg);
				if (r == -1)
					return;
				if (r == 0) {
					disconnect(c, BD_NET_CLOSED,
					    "connection closed by peer");
					return;
				}
				if (r == -2) {
					disconnect(c, BD_NET_ERROR, emsg);
					return;
				}
				c->cin_len = (size_t)r;
				c->cin_pos = 0;
			}

			c->zs.next_in = c->cin + c->cin_pos;
			c->zs.avail_in = (unsigned)(c->cin_len - c->cin_pos);
			c->zs.next_out = scratch;
			c->zs.avail_out = sizeof scratch;
			zr = mz_inflate(&c->zs, MZ_NO_FLUSH);
			produced = sizeof scratch - c->zs.avail_out;
			c->cin_pos = c->cin_len - c->zs.avail_in;

			if (produced) {
				bd_telopt_recv(c->telopt, scratch, produced);
				c->want_inflate = 0;   /* no nested MCCP start */
				drain_pend(c);
				if (c->pend_sent < c->pend_len)
					return;
			}
			if (zr == MZ_STREAM_END) {
				mz_inflateEnd(&c->zs);
				c->zs_inited = 0;
				c->inflating = 0;
				c->cin_len = c->cin_pos = 0;
				continue;       /* back to plain reads */
			}
			if (zr != MZ_OK && zr != MZ_BUF_ERROR) {
				disconnect(c, BD_NET_ERROR,
				    "decompression error");
				return;
			}
			if (produced == 0 && c->zs.avail_in != 0) {
				/* stalled: corrupt stream */
				disconnect(c, BD_NET_ERROR,
				    "decompression stalled");
				return;
			}
			continue;
		}

		/* plain (uncompressed) read */
		{
			unsigned char buf[CHUNK];
			ssize_t r = transport_read(c, buf, sizeof buf,
			    emsg, sizeof emsg);
			size_t consumed;

			if (r == -1)
				return;
			if (r == 0) {
				disconnect(c, BD_NET_CLOSED,
				    "connection closed by peer");
				return;
			}
			if (r == -2) {
				disconnect(c, BD_NET_ERROR, emsg);
				return;
			}
			consumed = bd_telopt_recv(c->telopt, buf, (size_t)r);
			drain_pend(c);

			if (c->want_inflate) {
				size_t rem = (size_t)r - consumed;
				if (start_inflate(c) != 0) {
					disconnect(c, BD_NET_ERROR,
					    "inflate init failed");
					return;
				}
				if (rem > sizeof c->cin)
					rem = sizeof c->cin;
				memcpy(c->cin, buf + consumed, rem);
				c->cin_len = rem;
				c->cin_pos = 0;
			}
			if (c->pend_sent < c->pend_len)
				return;
		}
	}
}

static void
sock_cb(struct iox_loop *loop, int fd, unsigned events, void *arg)
{
	struct net_ctx *c = arg;
	(void)loop;
	(void)fd;

	if (c->connecting) {
		int err = 0;
		socklen_t el = sizeof err;
		if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &el) < 0 || err) {
			disconnect(c, BD_NET_ERROR,
			    err ? strerror(err) : "connect failed");
			return;
		}
		c->connecting = 0;
		if (c->tls)
			start_tls(c);           /* CONNECTED follows after handshake */
		else {
			update_interest(c);
			push_state(c, BD_NET_CONNECTED, NULL);
		}
		return;
	}

	if (c->handshaking) {
		do_handshake(c);
		return;
	}

	if (events & IOX_WRITE)
		flush_out(c);
	if (c->fd >= 0 && (events & IOX_READ))
		do_read(c);
	if (c->fd >= 0)
		update_interest(c);
}

static void
wake_cb(struct iox_loop *loop, int fd, unsigned events, void *arg)
{
	struct net_ctx *c = arg;
	unsigned char drain[64];
	uint8_t type;
	unsigned char buf[CHUNK];
	uint32_t len;
	(void)loop;
	(void)events;

	while (read(fd, drain, sizeof drain) > 0)
		;

	while (ring_get(c->n->tx, &type, buf, sizeof buf, &len)) {
		switch (type) {
		case MSG_CONNECT:
			do_connect(c, buf, len);
			break;
		case MSG_SEND:
			do_send(c, buf, len);
			break;
		case MSG_CLOSE:
			c->user_closed = 1;
			cancel_reconnect(c);    /* stop any pending retry */
			if (c->fd >= 0 ||
			    atomic_load(&c->n->state) != BD_NET_CLOSED) {
				close_socket(c);
				push_state(c, BD_NET_CLOSED, NULL);
			}
			break;
		case MSG_WINSIZE:
			if (len >= 4) {
				uint16_t cols, rows;
				memcpy(&cols, buf, 2);
				memcpy(&rows, buf + 2, 2);
				bd_telopt_set_winsize(c->telopt, cols, rows);
				update_interest(c);    /* NAWS may have queued out */
			}
			break;
		case MSG_TERMTYPE:
			buf[len < sizeof buf ? len : sizeof buf - 1] = '\0';
			bd_telopt_set_termtype(c->telopt, (char *)buf);
			break;
		}
	}

	if (atomic_load(&c->n->shutdown)) {
		cancel_reconnect(c);
		c->quit = 1;
		return;
	}

	/* UI may have drained rx: flush staged records and resume. With MCCP2
	 * there can be buffered compressed input to inflate even when the socket
	 * is not readable, so drive do_read directly rather than waiting on a
	 * socket event. */
	if (c->rx_blocked) {
		drain_pend(c);
		if (!c->rx_blocked && c->fd >= 0 &&
		    !c->connecting && !c->handshaking) {
			do_read(c);
			update_interest(c);
		}
	}
}

/* Candidate CA bundle locations, tried in order. The bundled Mozilla list at
 * share/birdie/cacert.pem is a packaging step (deferred); for now an explicit
 * override and the host's system bundle are honored. */
static const char *ca_paths[] = {
	NULL,                                   /* slot 0: $BIRDIE_CACERT */
	"/etc/ssl/certs/ca-certificates.crt",   /* Debian/Ubuntu */
	"/etc/pki/tls/certs/ca-bundle.crt",     /* RHEL/Fedora */
	"/etc/ssl/cert.pem",                    /* BSD/macOS */
};

/* One-time TLS setup for the thread: RNG, trust store, client config. Leaves
 * c->tls_ready set on success; a failed connect reports if TLS is unavailable. */
static void
tls_global_init(struct net_ctx *c)
{
	const char *pers = "birdie-net";
	size_t i;
	int rc, loaded = 0;

	c->tls_insecure = getenv("BIRDIE_TLS_INSECURE") != NULL;

	if (psa_crypto_init() != PSA_SUCCESS)     /* required for TLS 1.3 */
		return;

	mbedtls_entropy_init(&c->entropy);
	mbedtls_ctr_drbg_init(&c->drbg);
	mbedtls_x509_crt_init(&c->cacert);
	mbedtls_ssl_config_init(&c->conf);

	rc = mbedtls_ctr_drbg_seed(&c->drbg, mbedtls_entropy_func, &c->entropy,
	    (const unsigned char *)pers, strlen(pers));
	if (rc != 0)
		return;

	ca_paths[0] = getenv("BIRDIE_CACERT");
	for (i = 0; i < sizeof ca_paths / sizeof ca_paths[0]; i++) {
		if (ca_paths[i] &&
		    mbedtls_x509_crt_parse_file(&c->cacert, ca_paths[i]) == 0) {
			loaded = 1;
			break;
		}
	}

	if (mbedtls_ssl_config_defaults(&c->conf, MBEDTLS_SSL_IS_CLIENT,
	    MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT) != 0)
		return;
	mbedtls_ssl_conf_authmode(&c->conf, c->tls_insecure
	    ? MBEDTLS_SSL_VERIFY_NONE : MBEDTLS_SSL_VERIFY_REQUIRED);
	if (loaded)
		mbedtls_ssl_conf_ca_chain(&c->conf, &c->cacert, NULL);
	mbedtls_ssl_conf_rng(&c->conf, mbedtls_ctr_drbg_random, &c->drbg);

	c->tls_ready = 1;
}

static void
tls_global_free(struct net_ctx *c)
{
	if (!c->tls_ready)
		return;
	mbedtls_ssl_config_free(&c->conf);
	mbedtls_x509_crt_free(&c->cacert);
	mbedtls_ctr_drbg_free(&c->drbg);
	mbedtls_entropy_free(&c->entropy);
}

static void
net_thread_main_inner(bd_net *n, struct net_ctx *c)
{
	bd_telopt_cb tcb;

	memset(c, 0, sizeof *c);
	c->n = n;
	c->fd = -1;
	c->reconnect_timer = -1;
	c->backoff_ms = RECONNECT_BASE_MS;
	tls_global_init(c);

	tcb.data = telopt_data;
	tcb.xmit = telopt_xmit;
	tcb.prompt = telopt_prompt;
	tcb.echo = telopt_echo;
	tcb.package = telopt_package;
	tcb.compress = telopt_compress;
	tcb.arg = c;

	c->loop = iox_loop_new();
	if (!c->loop)
		return;
	c->telopt = bd_telopt_new(&tcb);
	if (!c->telopt) {
		iox_loop_free(c->loop);
		c->loop = NULL;
		return;
	}
	iox_fd_add(c->loop, n->wake_r, IOX_READ, wake_cb, c);
	iox_loop_start(c->loop);

	while (!c->quit) {
		if (iox_loop_poll(c->loop) < 0 && errno != EINTR)
			break;
	}
}

static void *
net_thread_main(void *arg)
{
	bd_net *n = arg;
	struct net_ctx c;

	net_thread_main_inner(n, &c);

	close_socket(&c);               /* frees ssl + closes fd if open */
	tls_global_free(&c);
	free(c.out);
	free(c.pend);
	bd_telopt_free(c.telopt);
	iox_loop_free(c.loop);
	return NULL;
}

/* ---- UI thread: public API ------------------------------------------- */

static int
make_pipe(int *rfd, int *wfd)
{
	int fds[2];
	if (pipe(fds) < 0)
		return -1;
	set_nonblock(fds[0]);
	set_nonblock(fds[1]);
	fcntl(fds[0], F_SETFD, FD_CLOEXEC);
	fcntl(fds[1], F_SETFD, FD_CLOEXEC);
	*rfd = fds[0];
	*wfd = fds[1];
	return 0;
}

bd_net *
bd_net_new(bd_net_data_cb on_data, bd_net_state_cb on_state, void *arg)
{
	bd_net *n = calloc(1, sizeof *n);
	if (!n)
		return NULL;
	n->on_data = on_data;
	n->on_state = on_state;
	n->arg = arg;
	n->wake_r = n->wake_w = -1;
	atomic_init(&n->state, BD_NET_IDLE);
	atomic_init(&n->shutdown, 0);
	atomic_init(&n->autoreconnect, 1);

	n->rx = bd_ring_new(RX_CAP);
	n->tx = bd_ring_new(TX_CAP);
	if (!n->rx || !n->tx || make_pipe(&n->wake_r, &n->wake_w) < 0)
		goto fail;
	if (pthread_create(&n->thread, NULL, net_thread_main, n) != 0)
		goto fail;
	n->have_thread = 1;
	return n;

fail:
	if (n->wake_r >= 0)
		close(n->wake_r);
	if (n->wake_w >= 0)
		close(n->wake_w);
	bd_ring_free(n->rx);
	bd_ring_free(n->tx);
	free(n);
	return NULL;
}

void
bd_net_free(bd_net *n)
{
	if (!n)
		return;
	if (n->have_thread) {
		atomic_store(&n->shutdown, 1);
		wake(n);
		pthread_join(n->thread, NULL);
	}
	if (n->wake_r >= 0)
		close(n->wake_r);
	if (n->wake_w >= 0)
		close(n->wake_w);
	bd_ring_free(n->rx);
	bd_ring_free(n->tx);
	free(n);
}

void
bd_net_set_echo_cb(bd_net *n, bd_net_echo_cb cb)
{
	if (n)
		n->on_echo = cb;
}

void
bd_net_set_prompt_cb(bd_net *n, bd_net_prompt_cb cb)
{
	if (n)
		n->on_prompt = cb;
}

void
bd_net_set_package_cb(bd_net *n, bd_net_package_cb cb)
{
	if (n)
		n->on_package = cb;
}

void
bd_net_set_autoreconnect(bd_net *n, int enable)
{
	if (n)
		atomic_store(&n->autoreconnect, enable ? 1 : 0);
}

int
bd_net_connect(bd_net *n, const char *host, const char *port, int tls)
{
	unsigned char payload[512];
	size_t hl, pl;

	if (!n || !host || !port)
		return -1;
	hl = strlen(host);
	pl = strlen(port);
	if (1 + hl + pl + 2 > sizeof payload)
		return -1;
	payload[0] = tls ? 1 : 0;
	memcpy(payload + 1, host, hl + 1);
	memcpy(payload + 1 + hl + 1, port, pl + 1);

	if (ring_put(n->tx, MSG_CONNECT, payload, (uint32_t)(1 + hl + pl + 2)) < 0)
		return -1;
	wake(n);
	return 0;
}

int
bd_net_send(bd_net *n, const void *data, int len)
{
	const unsigned char *p = data;
	int left = len;

	if (!n || len < 0)
		return -1;
	if (atomic_load(&n->state) != BD_NET_CONNECTED)
		return -1;

	while (left > 0) {
		uint32_t chunk = left > CHUNK ? CHUNK : (uint32_t)left;
		if (ring_put(n->tx, MSG_SEND, p, chunk) < 0) {
			wake(n);
			return len - left;
		}
		p += chunk;
		left -= (int)chunk;
	}
	wake(n);
	return len;
}

void
bd_net_close(bd_net *n)
{
	if (!n)
		return;
	if (ring_put(n->tx, MSG_CLOSE, NULL, 0) == 0)
		wake(n);
}

void
bd_net_set_termtype(bd_net *n, const char *type)
{
	size_t l;

	if (!n || !type)
		return;
	l = strlen(type);
	if (l > 63)
		l = 63;
	if (ring_put(n->tx, MSG_TERMTYPE, type, (uint32_t)l) == 0)
		wake(n);
}

void
bd_net_set_winsize(bd_net *n, int cols, int rows)
{
	unsigned char p[4];
	uint16_t c = (uint16_t)cols, r = (uint16_t)rows;

	if (!n || cols <= 0 || rows <= 0)
		return;
	memcpy(p, &c, 2);
	memcpy(p + 2, &r, 2);
	if (ring_put(n->tx, MSG_WINSIZE, p, sizeof p) == 0)
		wake(n);
}

void
bd_net_poll(bd_net *n)
{
	int got_data = 0;

	if (!n)
		return;

	for (;;) {
		unsigned char hdr[5];
		unsigned char stackbuf[4096];
		unsigned char *p = stackbuf;
		uint8_t type;
		uint32_t len;

		if (bd_ring_read_avail(n->rx) < sizeof hdr)
			break;
		bd_ring_peek(n->rx, hdr, sizeof hdr);
		memcpy(&len, hdr + 1, sizeof len);
		if (bd_ring_read_avail(n->rx) < sizeof hdr + (size_t)len)
			break;          /* record not fully arrived yet */

		/* size the buffer to the whole record, plus a trailing NUL so
		 * string payloads (state detail, package json) are terminated */
		if ((size_t)len + 1 > sizeof stackbuf) {
			p = malloc((size_t)len + 1);
			if (!p) {
				bd_ring_skip(n->rx, sizeof hdr + len);
				continue;
			}
		}
		type = hdr[0];
		bd_ring_skip(n->rx, sizeof hdr);
		bd_ring_read(n->rx, p, len);
		p[len] = '\0';

		switch (type) {
		case MSG_DATA:
			if (n->on_data)
				n->on_data((const char *)p, (int)len, n->arg);
			got_data = 1;
			break;
		case MSG_STATE:
			if (n->on_state) {
				const char *msg = len > 1 ? (const char *)(p + 1)
				                          : NULL;
				n->on_state((bd_net_state)p[0], msg, n->arg);
			}
			break;
		case MSG_PROMPT:
			if (n->on_prompt)
				n->on_prompt(n->arg);
			break;
		case MSG_ECHO:
			if (n->on_echo)
				n->on_echo(len >= 1 && p[0], n->arg);
			break;
		case MSG_PKG:
			if (n->on_package && len >= 2) {
				const char *name = (const char *)(p + 1);
				size_t nl = strlen(name);
				const char *json = (const char *)(p + 1 + nl + 1);
				n->on_package(p[0], name, json, n->arg);
			}
			break;
		}

		if (p != stackbuf)
			free(p);
	}

	if (got_data)
		wake(n);        /* nudge the net thread to lift backpressure */
}

bd_net_state
bd_net_state_get(const bd_net *n)
{
	if (!n)
		return BD_NET_IDLE;
	return (bd_net_state)atomic_load((_Atomic int *)&n->state);
}
