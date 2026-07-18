/*
 * Loopback integration test for bd_net -- the real network layer.
 *
 * Unlike test_session (which fakes bd_net), this exercises the genuine
 * bd_net: its background net thread, the libiox poll loop, the self-pipe wake,
 * the rx/tx rings, and the telnet option layer, all against a real TCP socket.
 * The test opens a local listener, points bd_net at it, plays the server end by
 * hand (writing telnet + application bytes, reading what bd_net sends back), and
 * asserts the decoded callbacks and negotiation responses. Plaintext only (no
 * TLS); TLS needs a certificate exchange out of scope here.
 *
 * Everything is bounded by timeouts and poll() loops so a stuck socket fails a
 * check instead of hanging CI. Exit code 0 = all checks passed. Run via
 * `make test`.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */
#define _GNU_SOURCE          /* memmem */
#include "bd_net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/pk.h>
#include <mbedtls/net_sockets.h>
#include <psa/crypto.h>

/* ---- test harness ---- */
static int checks, fails;
static void
check(const char *what, int ok)
{
	checks++;
	if (!ok) fails++;
	printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
}

/* A self-signed EC cert + key for the in-test TLS server. The client runs with
 * BIRDIE_TLS_INSECURE so it never verifies, hence no CA or expiry concern.
 * Regenerate with:
 *   openssl ecparam -genkey -name prime256v1 -noout -out k.pem
 *   openssl req -new -x509 -key k.pem -days 36500 -subj /CN=birdie-test -out c.pem */
static const char TEST_CERT[] =
	"-----BEGIN CERTIFICATE-----\n"
	"MIIBgjCCASmgAwIBAgIULujiDyBo72K9lWohk9h0TXkoAZkwCgYIKoZIzj0EAwIw\n"
	"FjEUMBIGA1UEAwwLYmlyZGllLXRlc3QwIBcNMjYwNzE4MDMzMTQzWhgPMjEyNjA2\n"
	"MjQwMzMxNDNaMBYxFDASBgNVBAMMC2JpcmRpZS10ZXN0MFkwEwYHKoZIzj0CAQYI\n"
	"KoZIzj0DAQcDQgAEpqQcvpIAl9FyPnyh7s2XeV5Ju5r1QpS0RHwftUvKdgEGEKJh\n"
	"lDiUPJiJjn10K8H/QKUFDgGbQafqoaiLZ8PteqNTMFEwHQYDVR0OBBYEFKTZChp2\n"
	"GyZzb5xJseujvFJKh2ZYMB8GA1UdIwQYMBaAFKTZChp2GyZzb5xJseujvFJKh2ZY\n"
	"MA8GA1UdEwEB/wQFMAMBAf8wCgYIKoZIzj0EAwIDRwAwRAIgQyyFqTRzgc+yWSGi\n"
	"8WGg9fEbRe6mgpOGwCknowGa4sICIBOIMf4Yymh9mMriPQoaafDbM2+vV1K9R+9Y\n"
	"4e/3vku8\n"
	"-----END CERTIFICATE-----\n"
	;
static const char TEST_KEY[] =
	"-----BEGIN EC PRIVATE KEY-----\n"
	"MHcCAQEEIKIzH/6Ad1cZJQttD8DKDkjnsX/N7UW010dF2UYLGlwFoAoGCCqGSM49\n"
	"AwEHoUQDQgAEpqQcvpIAl9FyPnyh7s2XeV5Ju5r1QpS0RHwftUvKdgEGEKJhlDiU\n"
	"PJiJjn10K8H/QKUFDgGbQafqoaiLZ8Pteg==\n"
	"-----END EC PRIVATE KEY-----\n"
	;

static long
now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

/* ---- bd_net callback recorders (fired on the UI thread during poll) ---- */
static unsigned char rx[8192];
static size_t        rx_n;
static bd_net_state  st_last = BD_NET_IDLE;
static int           echo_last = -1, echo_n;
static int           prompt_n;

static void on_data(const char *d, int len, void *a)
{ (void)a; if (len > 0 && rx_n + (size_t)len <= sizeof rx) { memcpy(rx + rx_n, d, (size_t)len); rx_n += (size_t)len; } }
static void on_state(bd_net_state s, const char *msg, void *a) { (void)msg; (void)a; st_last = s; }
static void on_echo(int suppress, void *a) { (void)a; echo_n++; echo_last = suppress; }
static void on_prompt(void *a) { (void)a; prompt_n++; }

static int rx_has(const void *p, size_t n)
{ return memmem(rx, rx_n, p, n) != NULL; }

/* ---- server side: accumulate what bd_net transmits ---- */
static unsigned char srv[8192];
static size_t        srv_n;

/* read from the server socket until it holds `needle`, or timeout. Bounded so a
 * missing response fails a check rather than blocking. */
static int
srv_wait(int fd, const void *needle, size_t nl, int timeout_ms)
{
	long dl = now_ms() + timeout_ms;
	for (;;) {
		if (srv_n >= nl && memmem(srv, srv_n, needle, nl))
			return 1;
		long rem = dl - now_ms();
		if (rem <= 0)
			return 0;
		struct pollfd pf = { fd, POLLIN, 0 };
		if (poll(&pf, 1, (int)rem) <= 0)
			return 0;
		ssize_t r = recv(fd, srv + srv_n, sizeof srv - srv_n, 0);
		if (r <= 0)
			return 0;
		srv_n += (size_t)r;
	}
}

/* pump bd_net_poll until the recorder predicate holds, or timeout. */
#define PUMP_UNTIL(n, cond, ms) do {                            \
	long _dl = now_ms() + (ms);                             \
	while (now_ms() < _dl && !(cond)) {                     \
		bd_net_poll(n);                                 \
		usleep(2000);                                   \
	}                                                       \
	bd_net_poll(n);                                         \
} while (0)

int
main(void)
{
	signal(SIGPIPE, SIG_IGN);
	printf("bd_net (loopback):\n");

	/* a local listener on an ephemeral port */
	int lsn = socket(AF_INET, SOCK_STREAM, 0);
	int one = 1;
	setsockopt(lsn, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	struct sockaddr_in sa = { 0 };
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	sa.sin_port = 0;
	if (bind(lsn, (struct sockaddr *)&sa, sizeof sa) != 0 || listen(lsn, 1) != 0) {
		printf("  [FAIL] could not open a loopback listener\n");
		return 1;
	}
	socklen_t sl = sizeof sa;
	getsockname(lsn, (struct sockaddr *)&sa, &sl);
	char port[16];
	snprintf(port, sizeof port, "%d", ntohs(sa.sin_port));

	bd_net *n = bd_net_new(on_data, on_state, NULL);
	check("bd_net created (net thread started)", n != NULL);
	bd_net_set_echo_cb(n, on_echo);
	bd_net_set_prompt_cb(n, on_prompt);
	bd_net_set_autoreconnect(n, 0);

	check("connect attempt starts",
	    bd_net_connect(n, "127.0.0.1", port, 0) == 0);

	/* accept the connection bd_net's thread makes (bounded) */
	struct pollfd pf = { lsn, POLLIN, 0 };
	int srvfd = -1;
	if (poll(&pf, 1, 3000) > 0)
		srvfd = accept(lsn, NULL, NULL);
	check("bd_net connects to the listener", srvfd >= 0);

	PUMP_UNTIL(n, st_last == BD_NET_CONNECTED, 3000);
	check("the CONNECTED state is delivered via poll",
	    st_last == BD_NET_CONNECTED &&
	    bd_net_state_get(n) == BD_NET_CONNECTED);

	if (srvfd >= 0) {
		/* application text arrives as decoded data */
		send(srvfd, "hello world\r\n", 13, MSG_NOSIGNAL);
		PUMP_UNTIL(n, rx_has("hello world", 11), 3000);
		check("received text is decoded to the data callback",
		    rx_has("hello world", 11));

		/* telnet: DO SGA -> bd_net answers WILL SGA on the wire */
		{ unsigned char m[] = { 255, 253, 3 };   /* IAC DO SGA */
		  send(srvfd, m, sizeof m, MSG_NOSIGNAL); }
		{ unsigned char want[] = { 255, 251, 3 };/* IAC WILL SGA */
		  check("bd_net answers DO SGA with WILL SGA over the socket",
		      srv_wait(srvfd, want, sizeof want, 3000)); }

		/* telnet: WILL ECHO -> echo callback (suppress local echo) */
		{ unsigned char m[] = { 255, 251, 1 };   /* IAC WILL ECHO */
		  send(srvfd, m, sizeof m, MSG_NOSIGNAL); }
		PUMP_UNTIL(n, echo_last == 1, 3000);
		check("server WILL ECHO fires the echo callback (suppress=1)",
		    echo_n >= 1 && echo_last == 1);

		/* escaped IAC (0xFF 0xFF) decodes to a single literal 0xFF in data */
		rx_n = 0;
		{ unsigned char m[] = { 'A', 255, 255, 'B' };
		  send(srvfd, m, sizeof m, MSG_NOSIGNAL); }
		{ unsigned char want[] = { 'A', 255, 'B' };
		  PUMP_UNTIL(n, rx_has(want, sizeof want), 3000);
		  check("an escaped IAC IAC decodes to one 0xFF byte",
		      rx_has(want, sizeof want)); }

		/* legacy encoding: with a Latin-1 fallback selected, a high byte
		 * (0xE9 = 'é') is transcoded to 2-byte UTF-8 before the callback */
		bd_net_set_encoding(n, "ISO-8859-1");
		rx_n = 0;
		{ unsigned char m[] = { 'c', 'a', 'f', 0xE9 };
		  send(srvfd, m, sizeof m, MSG_NOSIGNAL); }
		{ unsigned char want[] = { 'c', 'a', 'f', 0xC3, 0xA9 };
		  PUMP_UNTIL(n, rx_has(want, sizeof want), 3000);
		  check("a Latin-1 byte is transcoded to UTF-8 for the display",
		      rx_has(want, sizeof want)); }
		bd_net_set_encoding(n, "UTF-8");         /* restore passthrough */

		/* telnet GA marks a prompt boundary */
		{ unsigned char m[] = { 255, 249 };      /* IAC GA */
		  send(srvfd, m, sizeof m, MSG_NOSIGNAL); }
		PUMP_UNTIL(n, prompt_n >= 1, 3000);
		check("a telnet GA fires the prompt callback", prompt_n >= 1);

		/* outbound: bd_net_send writes to the socket */
		srv_n = 0;
		check("bd_net_send accepts bytes",
		    bd_net_send(n, "quit\r\n", 6) == 6);
		check("sent bytes reach the server socket",
		    srv_wait(srvfd, "quit\r\n", 6, 3000));

		/* peer close -> bd_net reports CLOSED */
		close(srvfd);
		PUMP_UNTIL(n, st_last == BD_NET_CLOSED || st_last == BD_NET_ERROR, 3000);
		check("a peer close surfaces as CLOSED",
		    st_last == BD_NET_CLOSED || st_last == BD_NET_ERROR);
	}

	bd_net_free(n);        /* joins the net thread */
	close(lsn);

	/* ---- TLS: a real handshake + encrypted round trip against an in-test
	 * mbedTLS server (the client verifies nothing, per BIRDIE_TLS_INSECURE) ---- */
	setenv("BIRDIE_TLS_INSECURE", "1", 1);   /* set before bd_net_new: read on the thread */
	{
	int lsn2 = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in sa2; socklen_t sl2 = sizeof sa2;
	memset(&sa2, 0, sizeof sa2);
	sa2.sin_family = AF_INET;
	sa2.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (lsn2 < 0 || bind(lsn2, (struct sockaddr *)&sa2, sizeof sa2) != 0 ||
	    listen(lsn2, 1) != 0 ||
	    getsockname(lsn2, (struct sockaddr *)&sa2, &sl2) != 0) {
		check("TLS: opened a loopback listener", 0);
	} else {
		char port2[16];
		bd_net *nt;
		struct pollfd pf2 = { lsn2, POLLIN, 0 };
		int cfd = -1;
		snprintf(port2, sizeof port2, "%d", ntohs(sa2.sin_port));

		rx_n = 0; srv_n = 0; st_last = BD_NET_IDLE;
		nt = bd_net_new(on_data, on_state, NULL);
		check("TLS: bd_net created", nt != NULL);
		check("TLS: connect(tls=1) attempt starts",
		    bd_net_connect(nt, "127.0.0.1", port2, 1) == 0);
		if (poll(&pf2, 1, 3000) > 0)
			cfd = accept(lsn2, NULL, NULL);
		check("TLS: client connects to the listener", cfd >= 0);

		if (cfd >= 0) {
			mbedtls_ssl_context ssl; mbedtls_ssl_config conf;
			mbedtls_x509_crt cert; mbedtls_pk_context pk;
			mbedtls_entropy_context ent; mbedtls_ctr_drbg_context drbg;
			mbedtls_net_context net;
			int rc; long dl;

			mbedtls_ssl_init(&ssl); mbedtls_ssl_config_init(&conf);
			mbedtls_x509_crt_init(&cert); mbedtls_pk_init(&pk);
			mbedtls_entropy_init(&ent); mbedtls_ctr_drbg_init(&drbg);
			psa_crypto_init();               /* idempotent; needed for TLS 1.3 */
			mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &ent,
			    (const unsigned char *)"birdie-tls-test", 15);
			rc = mbedtls_x509_crt_parse(&cert,
			    (const unsigned char *)TEST_CERT, sizeof TEST_CERT);
			check("TLS: server cert parses", rc == 0);
			rc = mbedtls_pk_parse_key(&pk,
			    (const unsigned char *)TEST_KEY, sizeof TEST_KEY,
			    NULL, 0, mbedtls_ctr_drbg_random, &drbg);
			check("TLS: server key parses", rc == 0);
			mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_SERVER,
			    MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
			mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
			mbedtls_ssl_conf_own_cert(&conf, &cert, &pk);
			mbedtls_ssl_setup(&ssl, &conf);
			net.fd = cfd;
			mbedtls_net_set_nonblock(&net);
			mbedtls_ssl_set_bio(&ssl, &net, mbedtls_net_send,
			    mbedtls_net_recv, NULL);

			/* the client handshakes on its own thread; yield on WANT_* */
			dl = now_ms() + 8000;
			while ((rc = mbedtls_ssl_handshake(&ssl)) != 0) {
				if ((rc == MBEDTLS_ERR_SSL_WANT_READ ||
				     rc == MBEDTLS_ERR_SSL_WANT_WRITE) &&
				    now_ms() < dl) { usleep(2000); continue; }
				break;
			}
			check("TLS: server-side handshake completes", rc == 0);
			PUMP_UNTIL(nt, st_last == BD_NET_CONNECTED, 3000);
			check("TLS: client reports CONNECTED after the handshake",
			    st_last == BD_NET_CONNECTED);

			/* server -> client ciphertext decrypts to the data callback */
			rx_n = 0;
			mbedtls_ssl_write(&ssl, (const unsigned char *)"secret\r\n", 8);
			PUMP_UNTIL(nt, rx_has("secret", 6), 3000);
			check("TLS: server ciphertext is decrypted to the client",
			    rx_has("secret", 6));

			/* client -> server: bd_net_send is encrypted; server decrypts it */
			bd_net_send(nt, "hello\r\n", 7);
			{
				unsigned char b[256]; int got = 0;
				dl = now_ms() + 3000;
				while (now_ms() < dl && !memmem(b, (size_t)got, "hello", 5)) {
					int r = mbedtls_ssl_read(&ssl, b + got,
					    sizeof b - (size_t)got);
					if (r > 0) got += r;
					else usleep(2000);
				}
				check("TLS: client ciphertext decrypts on the server",
				    memmem(b, (size_t)got, "hello", 5) != NULL);
			}

			mbedtls_ssl_close_notify(&ssl);
			mbedtls_ssl_free(&ssl); mbedtls_ssl_config_free(&conf);
			mbedtls_x509_crt_free(&cert); mbedtls_pk_free(&pk);
			mbedtls_ctr_drbg_free(&drbg); mbedtls_entropy_free(&ent);
			close(cfd);
		}
		bd_net_free(nt);
	}
	close(lsn2);
	}

	printf("\n%d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
}
