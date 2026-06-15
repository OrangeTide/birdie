/*
 * bd_net -- birdie's network connection layer (increment 1: plain TCP/telnet).
 *
 * POSIX sockets only for now; a Winsock path and the threaded libiox model
 * from doc/network.md are deferred. See bd_net.h for the contract.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#include "bd_net.h"

#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>

/* telnet protocol bytes (RFC 854 + friends) */
#define TN_SE   240
#define TN_GA   249
#define TN_SB   250
#define TN_WILL 251
#define TN_WONT 252
#define TN_DO   253
#define TN_DONT 254
#define TN_IAC  255

/* inbound telnet parser states */
enum tn_state {
	TS_DATA,        /* ordinary data */
	TS_IAC,         /* saw IAC */
	TS_OPT,         /* saw IAC <WILL|WONT|DO|DONT>, next byte is the option */
	TS_SB,          /* inside subnegotiation, swallowing */
	TS_SB_IAC       /* inside subnegotiation, saw IAC */
};

struct bd_net {
	int fd;
	bd_net_state state;
	bd_net_data_cb on_data;
	bd_net_state_cb on_state;
	void *arg;

	/* inbound telnet parse */
	enum tn_state ts;
	unsigned char tn_cmd;   /* the WILL/WONT/DO/DONT awaiting its option */

	/* outbound queue: bytes [sent, len) still owed to the socket */
	unsigned char *out;
	int out_len;
	int out_cap;
	int out_sent;
};

static void
set_state(bd_net *n, bd_net_state s, const char *msg)
{
	n->state = s;
	if (n->on_state)
		n->on_state(s, msg, n->arg);
}

static void
close_fd(bd_net *n)
{
	if (n->fd >= 0) {
		close(n->fd);
		n->fd = -1;
	}
	n->ts = TS_DATA;
	n->out_len = n->out_sent = 0;
}

bd_net *
bd_net_new(bd_net_data_cb on_data, bd_net_state_cb on_state, void *arg)
{
	bd_net *n = calloc(1, sizeof *n);
	if (!n)
		return NULL;
	n->fd = -1;
	n->state = BD_NET_IDLE;
	n->on_data = on_data;
	n->on_state = on_state;
	n->arg = arg;
	n->ts = TS_DATA;
	return n;
}

void
bd_net_free(bd_net *n)
{
	if (!n)
		return;
	close_fd(n);
	free(n->out);
	free(n);
}

/* Append raw bytes to the outbound queue (no telnet escaping). Used for both
 * protocol replies and escaped user data. Returns 0 on success, -1 on OOM. */
static int
out_push(bd_net *n, const unsigned char *p, int len)
{
	if (n->out_sent == n->out_len)
		n->out_sent = n->out_len = 0;   /* queue drained: reset to front */
	if (n->out_len + len > n->out_cap) {
		int cap = n->out_cap ? n->out_cap * 2 : 256;
		while (cap < n->out_len + len)
			cap *= 2;
		unsigned char *nb = realloc(n->out, cap);
		if (!nb)
			return -1;
		n->out = nb;
		n->out_cap = cap;
	}
	memcpy(n->out + n->out_len, p, len);
	n->out_len += len;
	return 0;
}

/* Queue a 3-byte IAC <cmd> <opt> reply. */
static void
tn_reply(bd_net *n, unsigned char cmd, unsigned char opt)
{
	unsigned char r[3] = { TN_IAC, cmd, opt };
	out_push(n, r, sizeof r);
}

/* Feed raw socket bytes through the telnet filter, emitting clean application
 * data to the data callback. Policy: refuse every option (answer DO->WONT,
 * WILL->DONT), swallow subnegotiation and standalone commands like GA. */
static void
tn_feed(bd_net *n, const unsigned char *p, int len)
{
	unsigned char clean[4096];
	int c = 0;
	int i;

	for (i = 0; i < len; i++) {
		unsigned char b = p[i];
		switch (n->ts) {
		case TS_DATA:
			if (b == TN_IAC)
				n->ts = TS_IAC;
			else
				clean[c++] = b;
			break;
		case TS_IAC:
			if (b == TN_IAC) {              /* escaped 0xFF literal */
				clean[c++] = TN_IAC;
				n->ts = TS_DATA;
			} else if (b == TN_WILL || b == TN_WONT ||
			           b == TN_DO || b == TN_DONT) {
				n->tn_cmd = b;
				n->ts = TS_OPT;
			} else if (b == TN_SB) {
				n->ts = TS_SB;
			} else {                        /* GA/EOR/NOP/...: swallow */
				n->ts = TS_DATA;
			}
			break;
		case TS_OPT:
			if (n->tn_cmd == TN_DO)
				tn_reply(n, TN_WONT, b);
			else if (n->tn_cmd == TN_WILL)
				tn_reply(n, TN_DONT, b);
			/* WONT/DONT need no reply */
			n->ts = TS_DATA;
			break;
		case TS_SB:
			if (b == TN_IAC)
				n->ts = TS_SB_IAC;
			break;
		case TS_SB_IAC:
			if (b == TN_SE)
				n->ts = TS_DATA;
			else if (b == TN_IAC)
				n->ts = TS_SB;          /* literal 0xFF in SB data */
			else
				n->ts = TS_SB;          /* unexpected; resync */
			break;
		}

		if (c == (int)sizeof clean) {
			if (n->on_data)
				n->on_data((const char *)clean, c, n->arg);
			c = 0;
		}
	}
	if (c && n->on_data)
		n->on_data((const char *)clean, c, n->arg);
}

static int
set_nonblock(int fd)
{
	int fl = fcntl(fd, F_GETFL, 0);
	if (fl < 0)
		return -1;
	return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

int
bd_net_connect(bd_net *n, const char *host, const char *port)
{
	struct addrinfo hints, *res, *ai;
	int rc;

	bd_net_close(n);

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	/* Blocking resolution for now (brief); doc/network.md moves this to a
	 * worker thread with Happy Eyeballs. */
	rc = getaddrinfo(host, port, &hints, &res);
	if (rc != 0) {
		set_state(n, BD_NET_ERROR, gai_strerror(rc));
		return -1;
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
			n->fd = fd;
			freeaddrinfo(res);
			set_state(n, BD_NET_CONNECTED, NULL);
			return 0;
		}
		if (rc < 0 && errno == EINPROGRESS) {
			n->fd = fd;
			freeaddrinfo(res);
			set_state(n, BD_NET_CONNECTING, NULL);
			return 0;
		}
		close(fd);      /* this address refused immediately; try next */
	}

	freeaddrinfo(res);
	set_state(n, BD_NET_ERROR, "could not connect");
	return -1;
}

/* Try to flush the outbound queue. Returns 0 on progress/idle, -1 on a fatal
 * write error (caller transitions to ERROR). */
static int
flush_out(bd_net *n)
{
	while (n->out_sent < n->out_len) {
		ssize_t w = send(n->fd, n->out + n->out_sent,
		    (size_t)(n->out_len - n->out_sent), 0);
		if (w > 0) {
			n->out_sent += (int)w;
			continue;
		}
		if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			break;          /* socket buffer full; retry next poll */
		if (w < 0 && errno == EINTR)
			continue;
		return -1;
	}
	if (n->out_sent == n->out_len)
		n->out_sent = n->out_len = 0;
	return 0;
}

void
bd_net_poll(bd_net *n)
{
	if (!n || n->fd < 0)
		return;

	if (n->state == BD_NET_CONNECTING) {
		struct pollfd pfd = { n->fd, POLLOUT, 0 };
		if (poll(&pfd, 1, 0) <= 0)
			return;         /* not writable yet */
		int err = 0;
		socklen_t elen = sizeof err;
		if (getsockopt(n->fd, SOL_SOCKET, SO_ERROR, &err, &elen) < 0 || err) {
			close_fd(n);
			set_state(n, BD_NET_ERROR, err ? strerror(err) : "connect failed");
			return;
		}
		set_state(n, BD_NET_CONNECTED, NULL);
	}

	if (n->state != BD_NET_CONNECTED)
		return;

	if (flush_out(n) < 0) {
		close_fd(n);
		set_state(n, BD_NET_ERROR, strerror(errno));
		return;
	}

	for (;;) {
		unsigned char buf[4096];
		ssize_t r = recv(n->fd, buf, sizeof buf, 0);
		if (r > 0) {
			tn_feed(n, buf, (int)r);
			continue;
		}
		if (r == 0) {
			close_fd(n);
			set_state(n, BD_NET_CLOSED, "connection closed by peer");
			return;
		}
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			break;
		if (errno == EINTR)
			continue;
		close_fd(n);
		set_state(n, BD_NET_ERROR, strerror(errno));
		return;
	}

	/* The telnet filter may have queued option replies while reading;
	 * push them out now rather than waiting for the next poll. */
	if (flush_out(n) < 0) {
		close_fd(n);
		set_state(n, BD_NET_ERROR, strerror(errno));
	}
}

int
bd_net_send(bd_net *n, const void *data, int len)
{
	const unsigned char *p = data;
	int i, start;

	if (!n || n->state != BD_NET_CONNECTED || len < 0)
		return -1;

	/* Escape any 0xFF as IAC IAC so payload bytes are not mistaken for
	 * telnet commands. Push runs between escapes to keep it cheap. */
	for (i = 0, start = 0; i < len; i++) {
		if (p[i] == TN_IAC) {
			if (i > start)
				out_push(n, p + start, i - start);
			unsigned char esc[2] = { TN_IAC, TN_IAC };
			out_push(n, esc, 2);
			start = i + 1;
		}
	}
	if (len > start)
		out_push(n, p + start, len - start);

	flush_out(n);   /* opportunistic; poll() will finish any remainder */
	return len;
}

void
bd_net_close(bd_net *n)
{
	if (!n || n->fd < 0)
		return;
	close_fd(n);
	set_state(n, BD_NET_CLOSED, NULL);
}

bd_net_state
bd_net_state_get(const bd_net *n)
{
	return n ? n->state : BD_NET_IDLE;
}
