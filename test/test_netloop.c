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

/* ---- test harness ---- */
static int checks, fails;
static void
check(const char *what, int ok)
{
	checks++;
	if (!ok) fails++;
	printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
}

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

	printf("\n%d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
}
