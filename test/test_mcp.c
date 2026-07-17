/*
 * Headless tests for bd_mcp -- the MUD Client Protocol 2.1 core. bd_mcp has no
 * session or UI dependency (it is driven by callbacks), so this compiles it
 * directly and drives the handshake, negotiation, quoting, and the
 * dns-org-mud-moo-simpleedit round trip with recording callbacks.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */
#include "bd_mcp.h"

#include <stdio.h>
#include <string.h>

static int checks, fails;
static void
check(const char *what, int ok)
{
	checks++;
	if (!ok) fails++;
	printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
}

/* ---- recording callbacks ---- */
static char sent[64][512];
static int  nsent;
static void
rec_send(const char *line, void *ctx)
{
	(void)ctx;
	if (nsent < 64) {
		snprintf(sent[nsent], sizeof sent[0], "%s", line);
		nsent++;
	}
}
static int
sent_has(const char *sub)
{
	int i;
	for (i = 0; i < nsent; i++)
		if (strstr(sent[i], sub))
			return 1;
	return 0;
}

static char ed_ref[256], ed_name[128], ed_type[32], ed_content[1024];
static int  ed_n;
static void
rec_edit(const bd_mcp_edit *e, void *ctx)
{
	int n = e->content_len < (int)sizeof ed_content - 1
	    ? e->content_len : (int)sizeof ed_content - 1;
	(void)ctx;
	ed_n++;
	snprintf(ed_ref, sizeof ed_ref, "%s", e->reference);
	snprintf(ed_name, sizeof ed_name, "%s", e->name);
	snprintf(ed_type, sizeof ed_type, "%s", e->type);
	memcpy(ed_content, e->content, (size_t)n);
	ed_content[n] = '\0';
}

/* feed a NUL-terminated line; return the bd_mcp_feed result */
static int
feed(bd_mcp *m, const char *s)
{
	const char *out;
	int ol;
	return bd_mcp_feed(m, s, (int)strlen(s), &out, &ol);
}

int
main(void)
{
	bd_mcp_cb cb = { rec_send, rec_edit, NULL };
	bd_mcp *m = bd_mcp_new(&cb);
	const char *out;
	int ol, r;
	char line[512];
	const char *key;

	printf("bd_mcp tests\n");

	/* ordinary text passes through untouched */
	r = bd_mcp_feed(m, "hello world", 11, &out, &ol);
	check("ordinary line -> TEXT verbatim",
	    r == BD_MCP_TEXT && ol == 11 && memcmp(out, "hello world", 11) == 0);

	/* a #$# message before the handshake is consumed and ignored */
	r = feed(m, "#$#foo 123 key: val");
	check("pre-handshake message consumed, stays inactive",
	    r == BD_MCP_CONSUMED && !bd_mcp_active(m));

	/* handshake */
	nsent = 0;
	r = feed(m, "#$#mcp version: 2.1 to: 2.1");
	check("mcp handshake consumed + activates",
	    r == BD_MCP_CONSUMED && bd_mcp_active(m));
	check("reply sends an authentication-key",
	    sent_has("#$#mcp authentication-key:"));
	check("negotiates the mcp-negotiate package",
	    sent_has("package: mcp-negotiate"));
	check("negotiates the simpleedit package",
	    sent_has("package: dns-org-mud-moo-simpleedit"));
	check("ends negotiation", sent_has("#$#mcp-negotiate-end"));
	key = bd_mcp_authkey(m);
	check("auth key is non-empty", key[0] != '\0');

	/* a quoted ordinary line: strip the 3-byte marker, display the rest */
	{
		const char *q = "#$\"literal #$# text";
		r = bd_mcp_feed(m, q, (int)strlen(q), &out, &ol);
		check("quoted line -> TEXT with marker stripped",
		    r == BD_MCP_TEXT && ol == (int)strlen("literal #$# text") &&
		    memcmp(out, "literal #$# text", (size_t)ol) == 0);
	}

	/* wrong auth key: the message and its multiline body are dropped */
	ed_n = 0;
	feed(m, "#$#dns-org-mud-moo-simpleedit-content BADKEY reference: r "
	    "name: n type: string content*: \"\" _data-tag: bad");
	feed(m, "#$#* bad content: nope");
	feed(m, "#$#: bad");
	check("wrong auth key never opens an edit", ed_n == 0);

	/* correct simpleedit-content, multiline, dispatched on close */
	ed_n = 0;
	snprintf(line, sizeof line,
	    "#$#dns-org-mud-moo-simpleedit-content %s reference: \"#123:look\" "
	    "name: \"look msg\" type: string-list content*: \"\" _data-tag: t2",
	    key);
	feed(m, line);
	check("content header consumed, not dispatched yet", ed_n == 0);
	feed(m, "#$#* t2 content: line one");
	feed(m, "#$#* t2 content: line two");
	r = feed(m, "#$#: t2");
	check("close consumed + dispatches the edit",
	    r == BD_MCP_CONSUMED && ed_n == 1);
	check("edit reference parsed (quoted, with colon)",
	    strcmp(ed_ref, "#123:look") == 0);
	check("edit name parsed (quoted, with space)",
	    strcmp(ed_name, "look msg") == 0);
	check("edit type parsed", strcmp(ed_type, "string-list") == 0);
	check("edit content is the joined body",
	    strcmp(ed_content, "line one\nline two") == 0);

	/* send an edit back: simpleedit-set with the body split into lines */
	nsent = 0;
	bd_mcp_edit_done(m, "#123:look", "string-list", "new one\nnew two");
	check("set message sent",
	    sent_has("#$#dns-org-mud-moo-simpleedit-set"));
	check("set echoes the reference (quoted)",
	    sent_has("reference: \"#123:look\""));
	check("set body carries line one", sent_has("content: new one"));
	check("set body carries line two", sent_has("content: new two"));
	check("set closes its data tag", sent_has("#$#: "));

	/* reset clears the session */
	bd_mcp_reset(m);
	check("reset deactivates", !bd_mcp_active(m));
	nsent = 0;
	bd_mcp_edit_done(m, "r", "string", "x");
	check("edit_done is a no-op while inactive", nsent == 0);

	bd_mcp_free(m);

	printf("\n%d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
}
