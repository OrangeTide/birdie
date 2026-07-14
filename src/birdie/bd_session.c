/*
 * bd_session -- one connected MUD. See bd_session.h and doc/core.md.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#include "bd_session.h"
#include "bd_log.h"
#include "bd_replay.h"
#include "bd_mxp.h"
#include "bd_csv.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>

/* Monotonic milliseconds, for the trigger engine's interval timers. */
static double
mono_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
}

struct bd_session {
	const bd_profile *profile;      /* borrowed */
	bd_net *net;
	bd_vm *vm;
	bd_triggers *trig;
	bd_session_event_fn on_event;
	void *userdata;
	int cols, rows;
	char *data_dir;                 /* base for per-profile state, or NULL */

	bd_log *log;                    /* log sinks (NULL until data_dir set) */
	char session_id[25];            /* stable id for one session's records */
	int dispatch_depth;             /* re-entrancy guard for hook cascades */
	char *scratch_rd;               /* reusable buffer for file.read/list ret */
	bd_mxp *mxp;                    /* MXP tag parser (active when negotiated) */
	int mxp_active;                 /* server negotiated MXP */

	/* incoming-line assembly for the trigger engine (UI thread only) */
	char *linebuf;
	size_t line_len, line_cap;

	/* user triggers (GUI-edited, persisted to triggers.csv; see header) */
	struct user_trig *utrig;
	int nutrig, ucap;
};

/* One GUI-managed trigger; the persisted mirror of a live engine trigger. */
struct user_trig {
	bd_trigger_type type;
	char pattern[256];
	char body[256];
	char cls[64];
	int priority;
	int stop;
};

/* Canonical triggers.csv type names, indexed by bd_trigger_type. */
static const char *const trig_type_names[] = {
	"action", "alias", "prompt", "gmcp",
	"gag", "substitute", "highlight", "mxp"
};

static const char *
trig_type_name(bd_trigger_type t)
{
	if ((unsigned)t < sizeof trig_type_names / sizeof trig_type_names[0])
		return trig_type_names[t];
	return "action";
}

/* Parse a type name (case-sensitive, canonical form). Returns 0 on success. */
static int
trig_type_parse(const char *s, bd_trigger_type *out)
{
	size_t i;
	if (!s)
		return -1;
	for (i = 0; i < sizeof trig_type_names / sizeof trig_type_names[0]; i++) {
		if (strcmp(s, trig_type_names[i]) == 0) {
			*out = (bd_trigger_type)i;
			return 0;
		}
	}
	return -1;
}

/* A non-cryptographic, process-unique id used to tag a session's log records
 * (doc/logging.md). Stable for the life of the bd_session. */
static void
gen_session_id(char *out, size_t cap)
{
	struct timespec ts;
	static unsigned seq;
	clock_gettime(CLOCK_REALTIME, &ts);
	snprintf(out, cap, "%08lx%08lx%04x",
	    (unsigned long)ts.tv_sec, (unsigned long)ts.tv_nsec,
	    (unsigned)((getpid() ^ seq++) & 0xffff));
}

/* ---- per-profile persistent var table ---- */

/* mkdir -p path (best effort). */
static void
mkdir_p(const char *path)
{
	char tmp[1024];
	char *p;
	snprintf(tmp, sizeof tmp, "%s", path);
	for (p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			mkdir(tmp, 0755);
			*p = '/';
		}
	}
	mkdir(tmp, 0755);
}

/* Build <data_dir>/profiles/<name>/<fname> into buf. Returns 0 if a path
 * exists (data_dir set and profile has a name), -1 otherwise. */
static int
profile_file_path(bd_session *s, const char *fname, char *buf, size_t cap)
{
	const char *name = s->profile ? bd_profile_get(s->profile, "name") : NULL;
	char safe[128];
	size_t i;

	if (!s->data_dir || !name || !*name)
		return -1;
	for (i = 0; name[i] && i < sizeof safe - 1; i++)
		safe[i] = (name[i] == '/' || name[i] == '\\') ? '_' : name[i];
	safe[i] = '\0';
	snprintf(buf, cap, "%s/profiles/%s/%s", s->data_dir, safe, fname);
	return 0;
}

static int
vars_path(bd_session *s, char *buf, size_t cap)
{
	return profile_file_path(s, "vars.json", buf, cap);
}

/* Host function: __bd_savevars(json) -> write the var dump to disk. */
static int
host_savevars(bd_vm *vm, int argc, const bd_vm_val *argv, bd_vm_val *ret,
              void *ud)
{
	bd_session *s = ud;
	char path[1024], dir[1024];
	char *slash;
	FILE *f;
	(void)vm;
	(void)ret;

	if (argc < 1 || argv[0].type != BD_VM_STR)
		return 0;
	if (vars_path(s, path, sizeof path) != 0)
		return 0;
	snprintf(dir, sizeof dir, "%s", path);
	slash = strrchr(dir, '/');
	if (slash) {
		*slash = '\0';
		mkdir_p(dir);
	}
	f = fopen(path, "wb");
	if (!f)
		return 0;
	fwrite(argv[0].u.s, 1, strlen(argv[0].u.s), f);
	fclose(f);
	return 0;
}

/* Load the saved var table into the VM (no-op if the file is absent). */
static void
load_vars(bd_session *s)
{
	char path[1024];
	FILE *f;
	long sz;
	char *buf;
	size_t got;

	if (vars_path(s, path, sizeof path) != 0)
		return;
	f = fopen(path, "rb");
	if (!f)
		return;
	fseek(f, 0, SEEK_END);
	sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (sz <= 0) {
		fclose(f);
		return;
	}
	buf = malloc((size_t)sz + 1);
	if (!buf) {
		fclose(f);
		return;
	}
	got = fread(buf, 1, (size_t)sz, f);
	fclose(f);
	buf[got] = '\0';
	bd_vm_call(s->vm, "__bd_setvars", "s", buf);
	free(buf);
}

/* Persist the var table (no-op if no data dir / profile name). */
static void
save_vars(bd_session *s)
{
	char path[1024];
	if (vars_path(s, path, sizeof path) != 0)
		return;
	bd_vm_eval(s->vm, "__bd_savevars(json.encode(var))");
}

/* ---- confined per-profile scratch-dir file API (doc/triggers.md) ---- */

/* A scratch filename must be a single safe basename: non-empty, no path
 * separators, no "..", not hidden. This is the path-traversal guard. */
static int
valid_scratch_name(const char *name)
{
	size_t i;
	if (!name || !name[0] || name[0] == '.')
		return 0;
	for (i = 0; name[i]; i++) {
		char c = name[i];
		if (c == '/' || c == '\\')
			return 0;
		if (c == '.' && name[i + 1] == '.')
			return 0;
		if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
		    (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
			return 0;
	}
	return 1;
}

/* Build <data_dir>/profiles/<profile>/scratch[/<fname>] into buf. fname may be
 * NULL for just the directory. Returns 0 on success, -1 if there is no data
 * dir / profile name, or fname is unsafe. */
static int
scratch_path(bd_session *s, const char *fname, char *buf, size_t cap)
{
	const char *name = s->profile ? bd_profile_get(s->profile, "name") : NULL;
	char safe[128];
	size_t i;

	if (!s->data_dir || !name || !*name)
		return -1;
	if (fname && !valid_scratch_name(fname))
		return -1;
	for (i = 0; name[i] && i < sizeof safe - 1; i++)
		safe[i] = (name[i] == '/' || name[i] == '\\') ? '_' : name[i];
	safe[i] = '\0';
	if (fname)
		snprintf(buf, cap, "%s/profiles/%s/scratch/%s",
		    s->data_dir, safe, fname);
	else
		snprintf(buf, cap, "%s/profiles/%s/scratch", s->data_dir, safe);
	return 0;
}

/* __bd_file_write(name, data) / __bd_file_append: write to the scratch dir.
 * Returns a boolean: true on success. */
static int
file_put(bd_session *s, int argc, const bd_vm_val *argv, bd_vm_val *ret,
         const char *mode)
{
	char path[1024], dir[1024];
	FILE *f;

	*ret = bd_vm_bool(0);
	if (argc < 2 || argv[0].type != BD_VM_STR || argv[1].type != BD_VM_STR)
		return 0;
	if (scratch_path(s, argv[0].u.s, path, sizeof path) != 0)
		return 0;
	if (scratch_path(s, NULL, dir, sizeof dir) == 0)
		mkdir_p(dir);
	f = fopen(path, mode);
	if (!f)
		return 0;
	fwrite(argv[1].u.s, 1, strlen(argv[1].u.s), f);
	fclose(f);
	*ret = bd_vm_bool(1);
	return 0;
}

static int
host_file_write(bd_vm *vm, int argc, const bd_vm_val *argv, bd_vm_val *ret,
                void *ud)
{
	(void)vm;
	return file_put((bd_session *)ud, argc, argv, ret, "wb");
}

static int
host_file_append(bd_vm *vm, int argc, const bd_vm_val *argv, bd_vm_val *ret,
                 void *ud)
{
	(void)vm;
	return file_put((bd_session *)ud, argc, argv, ret, "ab");
}

/* __bd_file_read(name) -> file contents as a string, or nil. The result is
 * kept in s->scratch_rd, which stays valid until the next read/list or free. */
static int
host_file_read(bd_vm *vm, int argc, const bd_vm_val *argv, bd_vm_val *ret,
               void *ud)
{
	bd_session *s = ud;
	char path[1024];
	FILE *f;
	long sz;
	char *buf;
	size_t got;
	(void)vm;

	*ret = bd_vm_nil();
	if (argc < 1 || argv[0].type != BD_VM_STR)
		return 0;
	if (scratch_path(s, argv[0].u.s, path, sizeof path) != 0)
		return 0;
	f = fopen(path, "rb");
	if (!f)
		return 0;
	fseek(f, 0, SEEK_END);
	sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (sz < 0) {
		fclose(f);
		return 0;
	}
	buf = realloc(s->scratch_rd, (size_t)sz + 1);
	if (!buf) {
		fclose(f);
		return 0;
	}
	s->scratch_rd = buf;
	got = fread(buf, 1, (size_t)sz, f);
	fclose(f);
	buf[got] = '\0';
	*ret = bd_vm_str(buf);          /* push_val copies it into Lua */
	return 0;
}

/* __bd_file_remove(name) -> boolean success. */
static int
host_file_remove(bd_vm *vm, int argc, const bd_vm_val *argv, bd_vm_val *ret,
                 void *ud)
{
	bd_session *s = ud;
	char path[1024];
	(void)vm;

	*ret = bd_vm_bool(0);
	if (argc < 1 || argv[0].type != BD_VM_STR)
		return 0;
	if (scratch_path(s, argv[0].u.s, path, sizeof path) != 0)
		return 0;
	*ret = bd_vm_bool(remove(path) == 0);
	return 0;
}

/* __bd_file_list() -> newline-joined filenames in the scratch dir (Lua splits).
 * Result kept in s->scratch_rd. */
static int
host_file_list(bd_vm *vm, int argc, const bd_vm_val *argv, bd_vm_val *ret,
               void *ud)
{
	bd_session *s = ud;
	char dir[1024];
	DIR *d;
	struct dirent *e;
	size_t len = 0, cap = 256;
	char *buf;
	(void)vm;
	(void)argc;
	(void)argv;

	*ret = bd_vm_str("");
	if (scratch_path(s, NULL, dir, sizeof dir) != 0)
		return 0;
	d = opendir(dir);
	if (!d)
		return 0;
	buf = realloc(s->scratch_rd, cap);
	if (!buf) {
		closedir(d);
		return 0;
	}
	s->scratch_rd = buf;
	buf[0] = '\0';
	while ((e = readdir(d)) != NULL) {
		size_t nl = strlen(e->d_name);
		if (e->d_name[0] == '.')
			continue;
		if (len + nl + 2 > cap) {
			char *nb = realloc(buf, (cap = (len + nl + 2) * 2));
			if (!nb)
				break;
			buf = s->scratch_rd = nb;
		}
		memcpy(buf + len, e->d_name, nl);
		len += nl;
		buf[len++] = '\n';
	}
	closedir(d);
	buf[len] = '\0';
	*ret = bd_vm_str(buf);
	return 0;
}

/* truthy values for yes/no-style profile fields */
static int
prop_bool(const bd_profile *p, const char *key, int dflt)
{
	const char *v = bd_profile_get(p, key);
	if (!v)
		return dflt;
	if (!strcmp(v, "yes") || !strcmp(v, "true") || !strcmp(v, "1") ||
	    !strcmp(v, "on"))
		return 1;
	if (!strcmp(v, "no") || !strcmp(v, "false") || !strcmp(v, "0") ||
	    !strcmp(v, "off"))
		return 0;
	return dflt;
}

static void
emit(bd_session *s, const bd_session_event *ev)
{
	if (s->on_event)
		s->on_event(s, ev, s->userdata);
}

/* Prefill the common log fields for `kind` (mud / character / session / time).
 * Returns 0 if logging is active, -1 if there are no sinks (so callers can
 * skip building the payload). */
static int
log_fill(bd_session *s, bd_log_rec *r, bd_log_kind kind)
{
	if (!s->log)
		return -1;
	memset(r, 0, sizeof *r);
	r->kind = kind;
	r->t_ms = bd_log_now_ms();
	r->session = s->session_id;
	if (s->profile) {
		r->mud = bd_profile_get(s->profile, "name");
		r->character = bd_profile_get(s->profile, "character");
	}
	return 0;
}

/* Log an outbound command (send paths funnel through here). */
static void
log_send(bd_session *s, const char *text)
{
	bd_log_rec r;
	if (log_fill(s, &r, BD_LOG_SEND) != 0)
		return;
	r.raw = text;
	r.text = text;
	bd_log_write(s->log, &r);
}

/* Host function: __bd_note(text) -> write a `note` record (log.note in Lua). */
static int
host_note(bd_vm *vm, int argc, const bd_vm_val *argv, bd_vm_val *ret, void *ud)
{
	bd_session *s = ud;
	bd_log_rec r;
	(void)vm;
	(void)ret;
	if (argc < 1 || argv[0].type != BD_VM_STR || !argv[0].u.s)
		return 0;
	if (log_fill(s, &r, BD_LOG_NOTE) != 0)
		return 0;
	r.text = argv[0].u.s;
	bd_log_write(s->log, &r);
	return 0;
}

/* Host function: __bd_event(name, arg) -> raise a user event (event() in Lua).
 * Routes through the engine so the verb and script paths share one entry. */
static int
host_event(bd_vm *vm, int argc, const bd_vm_val *argv, bd_vm_val *ret, void *ud)
{
	bd_session *s = ud;
	(void)vm;
	(void)ret;
	if (argc >= 1 && argv[0].type == BD_VM_STR)
		bd_triggers_event(s->trig, argv[0].u.s,
		    (argc >= 2 && argv[1].type == BD_VM_STR) ? argv[1].u.s : "");
	return 0;
}

/* The trigger engine emits MUD commands through here. Send raw (with CRLF) so
 * a fired command does not recurse back through the aliases. */
static void
trigger_send(const char *cmd, void *ctx)
{
	bd_session *s = ctx;
	bd_net_send(s->net, cmd, (int)strlen(cmd));
	bd_net_send(s->net, "\r\n", 2);
	log_send(s, cmd);
}

/* Host function exposed to scripts: __bd_send(text) -> send a command line.
 * The mud.send(text) Lua wrapper (installed in the bootstrap below) calls it,
 * so '@' trigger bodies and #script can talk to the MUD. */
static int
host_send(bd_vm *vm, int argc, const bd_vm_val *argv, bd_vm_val *ret, void *ud)
{
	bd_session *s = ud;
	(void)vm;
	(void)ret;
	if (argc >= 1 && argv[0].type == BD_VM_STR && argv[0].u.s) {
		bd_net_send(s->net, argv[0].u.s, (int)strlen(argv[0].u.s));
		bd_net_send(s->net, "\r\n", 2);
		log_send(s, argv[0].u.s);
	}
	return 0;
}

/* Host function: __bd_gmcp(pkg, json) -> send an outbound GMCP package. */
static int
host_gmcp(bd_vm *vm, int argc, const bd_vm_val *argv, bd_vm_val *ret, void *ud)
{
	bd_session *s = ud;
	(void)vm;
	(void)ret;
	if (argc >= 1 && argv[0].type == BD_VM_STR)
		bd_net_send_gmcp(s->net, argv[0].u.s,
		    (argc >= 2 && argv[1].type == BD_VM_STR) ? argv[1].u.s : NULL);
	return 0;
}

/* Host function: __bd_class(name, enable) -> toggle a trigger class.
 * enable true=enable, false=disable, nil=toggle. */
static int
host_class(bd_vm *vm, int argc, const bd_vm_val *argv, bd_vm_val *ret, void *ud)
{
	bd_session *s = ud;
	const char *n;
	(void)vm;
	(void)ret;
	if (argc < 1 || argv[0].type != BD_VM_STR)
		return 0;
	n = argv[0].u.s;
	if (argc >= 2 && argv[1].type == BD_VM_BOOL) {
		if (argv[1].u.b)
			bd_class_enable(s->trig, n);
		else
			bd_class_disable(s->trig, n);
	} else {                                /* nil -> toggle */
		if (bd_class_enabled(s->trig, n))
			bd_class_disable(s->trig, n);
		else
			bd_class_enable(s->trig, n);
	}
	return 0;
}

/*
 * The script-facing API, built in Lua over the flat host functions, so the C
 * seam stays scalar. mud.send and class.* reach the host; the on.* hook tables
 * let scripts react to events alongside the #action/#alias triggers, dispatched
 * from C via __bd_dispatch. Each handler runs in pcall so one bad hook does not
 * abort the rest. GMCP/MSDP payloads are JSON-decoded (json.decode) to a Lua
 * table before the on.gmcp handler is called. log.note(text) writes a `note`
 * record to the log sinks (doc/logging.md). on.timer fires for each #tick that
 * elapses (the timer name is the argument). event(name, arg) raises a user
 * event, running on.event[name] (synchronous hook cascades are capped, see
 * dispatch_hook). watch(expr, fn) fires fn(new, old) when a watched Lua
 * expression changes value, polled once per frame from bd_session_drain. The
 * file.* table is a confined read/write/append/list/remove API rooted at the
 * per-profile scratch dir (the replacement for the sandboxed-away io).
 * Deferred (doc/triggers.md): on.mxp.
 */
static const char bootstrap_lua[] =
	"mud = mud or {}\n"
	"function mud.send(s) __bd_send(tostring(s)) end\n"
	"log = log or {}\n"
	"function log.note(s) __bd_note(tostring(s)) end\n"
	"function event(name, arg) __bd_event(tostring(name), arg==nil and '' or tostring(arg)) end\n"
	/* confined per-profile scratch file API (replaces the sandboxed-away io) */
	"file = file or {}\n"
	"function file.write(n, d) return __bd_file_write(tostring(n), tostring(d)) end\n"
	"function file.append(n, d) return __bd_file_append(tostring(n), tostring(d)) end\n"
	"function file.read(n) return __bd_file_read(tostring(n)) end\n"
	"function file.remove(n) return __bd_file_remove(tostring(n)) end\n"
	"function file.list()\n"
	"  local s = __bd_file_list(); local t = {}\n"
	"  for w in s:gmatch('[^\\n]+') do t[#t+1] = w end\n"
	"  return t\n"
	"end\n"
	"function mud.gmcp(pkg, data)\n"
	"  if data == nil then __bd_gmcp(pkg, nil)\n"
	"  elseif type(data)=='string' then __bd_gmcp(pkg, data)\n"
	"  else __bd_gmcp(pkg, json.encode(data)) end\n"
	"end\n"
	"class = class or {}\n"
	"function class.enable(n) __bd_class(n, true) end\n"
	"function class.disable(n) __bd_class(n, false) end\n"
	"function class.toggle(n) __bd_class(n, nil) end\n"
	/* a compact recursive-descent JSON decoder. byte-code comparisons keep
	 * the source free of quote/backslash escaping; json.decode returns the
	 * value or nil on malformed input. Used to hand GMCP/MSDP payloads to
	 * scripts as Lua tables. */
	"local function jdec(s)\n"
	" local p=1\n"
	" local function sk() while p<=#s do local b=s:byte(p)\n"
	"   if b==32 or b==9 or b==10 or b==13 then p=p+1 else break end end end\n"
	" local pv\n"
	" local function pstr()\n"
	"  p=p+1; local t={}\n"
	"  while p<=#s do local b=s:byte(p)\n"
	"   if b==34 then p=p+1; return table.concat(t) end\n"
	"   if b==92 then p=p+1; local e=s:byte(p)\n"
	"    if e==110 then t[#t+1]=string.char(10)\n"
	"    elseif e==116 then t[#t+1]=string.char(9)\n"
	"    elseif e==114 then t[#t+1]=string.char(13)\n"
	"    elseif e==98 then t[#t+1]=string.char(8)\n"
	"    elseif e==102 then t[#t+1]=string.char(12)\n"
	"    elseif e==117 then local h=tonumber(s:sub(p+1,p+4),16) or 63; p=p+4\n"
	"     if h<128 then t[#t+1]=string.char(h)\n"
	"     elseif h<2048 then t[#t+1]=string.char(192+h//64,128+h%64)\n"
	"     else t[#t+1]=string.char(224+h//4096,128+(h//64)%64,128+h%64) end\n"
	"    else t[#t+1]=string.char(e) end\n"
	"    p=p+1\n"
	"   else t[#t+1]=string.char(b); p=p+1 end end\n"
	"  error('eos')\n"
	" end\n"
	" pv=function()\n"
	"  sk(); local b=s:byte(p)\n"
	"  if b==123 then p=p+1; local o={}; sk(); if s:byte(p)==125 then p=p+1; return o end\n"
	"   while true do sk(); local k=pstr(); sk()\n"
	"    if s:byte(p)~=58 then error('colon') end; p=p+1; o[k]=pv(); sk()\n"
	"    local d=s:byte(p)\n"
	"    if d==44 then p=p+1 elseif d==125 then p=p+1; return o else error('obj') end end\n"
	"  elseif b==91 then p=p+1; local a={}; sk(); if s:byte(p)==93 then p=p+1; return a end\n"
	"   while true do a[#a+1]=pv(); sk(); local d=s:byte(p)\n"
	"    if d==44 then p=p+1 elseif d==93 then p=p+1; return a else error('arr') end end\n"
	"  elseif b==34 then return pstr()\n"
	"  elseif b==116 then p=p+4; return true\n"
	"  elseif b==102 then p=p+5; return false\n"
	"  elseif b==110 then p=p+4; return nil\n"
	"  else local st=p\n"
	"   while p<=#s do local c=s:byte(p)\n"
	"    if (c>=48 and c<=57) or c==45 or c==43 or c==46 or c==101 or c==69 then p=p+1 else break end end\n"
	"   return tonumber(s:sub(st,p-1)) end\n"
	" end\n"
	" return pv()\n"
	"end\n"
	"json = json or {}\n"
	"function json.decode(s) local ok,v=pcall(jdec,s); if ok then return v end return nil end\n"
	/* JSON encoder. byte codes avoid quote/backslash escaping in this C
	 * string; BS is a literal backslash. */
	"local BS = string.char(92)\n"
	"local function jstr(s)\n"
	" local r='\"'\n"
	" for i=1,#s do local b=s:byte(i)\n"
	"  if b==34 then r=r..BS..'\"'\n"
	"  elseif b==92 then r=r..BS..BS\n"
	"  elseif b==10 then r=r..BS..'n'\n"
	"  elseif b==9 then r=r..BS..'t'\n"
	"  elseif b==13 then r=r..BS..'r'\n"
	"  elseif b<32 then r=r..string.format(BS..'u%04x', b)\n"
	"  else r=r..string.char(b) end end\n"
	" return r..'\"'\n"
	"end\n"
	"local function jenc(v)\n"
	" local tp=type(v)\n"
	" if v==nil then return 'null'\n"
	" elseif tp=='boolean' then if v then return 'true' else return 'false' end\n"
	" elseif tp=='number' then return tostring(v)\n"
	" elseif tp=='string' then return jstr(v)\n"
	" elseif tp=='table' then\n"
	"  local n=0; for _ in pairs(v) do n=n+1 end\n"
	"  if n==0 then return '{}' end\n"
	"  if n==#v then local p={}; for i=1,#v do p[i]=jenc(v[i]) end\n"
	"   return '['..table.concat(p,',')..']'\n"
	"  else local p={}; for k,val in pairs(v) do p[#p+1]=jstr(tostring(k))..':'..jenc(val) end\n"
	"   return '{'..table.concat(p,',')..'}' end\n"
	" end\n"
	" return 'null'\n"
	"end\n"
	"json.encode = jenc\n"
	"var = var or {}\n"
	"function __bd_setvars(s) var = json.decode(s) or {} end\n"
	"on = on or { line={}, prompt={}, connect={}, disconnect={}, gmcp={}, timer={}, event={}, mxp={} }\n"
	/* MXP tags route to on.mxp[tag](attrs, closing) */
	"function __bd_mxp(tag, attrs, closing)\n"
	"  local h = on.mxp[tag]; if h then pcall(h, attrs, closing) end\n"
	"end\n"
	"function __bd_dispatch(kind, a, b)\n"
	"  if kind=='line' then for _,f in ipairs(on.line) do pcall(f,a) end\n"
	"  elseif kind=='prompt' then for _,f in ipairs(on.prompt) do pcall(f,a) end\n"
	"  elseif kind=='connect' then for _,f in ipairs(on.connect) do pcall(f) end\n"
	"  elseif kind=='disconnect' then for _,f in ipairs(on.disconnect) do pcall(f) end\n"
	"  elseif kind=='timer' then for _,f in ipairs(on.timer) do pcall(f,a) end\n"
	"  elseif kind=='event' then local h=on.event[a]\n"
	"   if h then pcall(h, b) end\n"
	/* hand GMCP/MSDP handlers the decoded table (nil if the payload was not
	 * valid JSON) */
	"  elseif kind=='gmcp' then local h=on.gmcp[a]\n"
	"   if h then pcall(h, json.decode(b)) end end\n"
	"end\n"
	/* expression watches: watch(expr, fn) fires fn(new, old) when a watched
	 * Lua expression's value changes. The first poll only records a baseline
	 * (no fire). expr may be a string (compiled as 'return <expr>') or a
	 * function. Comparison is by value, so watch scalars (number/string/bool).
	 * __bd_poll_expr is called once per frame from bd_session_drain. */
	"__watches = __watches or {}\n"
	"function watch(expr, fn)\n"
	"  local get\n"
	"  if type(expr)=='function' then get=expr else get=load('return '..expr) end\n"
	"  if not get then return false end\n"
	"  __watches[#__watches+1] = { get=get, fn=fn, last=nil, first=true }\n"
	"  return true\n"
	"end\n"
	"function __bd_poll_expr()\n"
	"  for _,w in ipairs(__watches) do\n"
	"    local ok, v = pcall(w.get)\n"
	"    if ok and (w.first or v ~= w.last) then\n"
	"      local old = w.last\n"
	"      w.last = v\n"
	"      if not w.first then pcall(w.fn, v, old) end\n"
	"      w.first = false\n"
	"    end\n"
	"  end\n"
	"end\n";

/*
 * The default sandbox (doc/triggers.md). Run once after the trusted bootstrap
 * so user scripts, profile scripts, #script, and any imported (untrusted) gist
 * scripts cannot reach process control, the filesystem, or sandbox-escape
 * hatches. The safe stdlib stays: string / table / math / utf8 / coroutine /
 * select / pairs / pcall / load (load's chunks inherit this hardened _ENV).
 * os keeps the read-only clocks (time / clock / date / difftime). A profile can
 * opt out with unsafe_scripts=yes for fully trusted local setups.
 */
static const char sandbox_lua[] =
	"if os then\n"
	"  os.execute=nil; os.remove=nil; os.rename=nil; os.exit=nil\n"
	"  os.tmpname=nil; os.getenv=nil; os.setlocale=nil\n"
	"end\n"
	"io=nil\n"                      /* no raw filesystem access (var persists state) */
	"dofile=nil; loadfile=nil\n"
	"package=nil; require=nil\n"
	"debug=nil\n";

static void
install_script_api(bd_session *s)
{
	bd_vm_register(s->vm, "__bd_send", host_send, s);
	bd_vm_register(s->vm, "__bd_class", host_class, s);
	bd_vm_register(s->vm, "__bd_gmcp", host_gmcp, s);
	bd_vm_register(s->vm, "__bd_savevars", host_savevars, s);
	bd_vm_register(s->vm, "__bd_note", host_note, s);
	bd_vm_register(s->vm, "__bd_event", host_event, s);
	bd_vm_register(s->vm, "__bd_file_write", host_file_write, s);
	bd_vm_register(s->vm, "__bd_file_append", host_file_append, s);
	bd_vm_register(s->vm, "__bd_file_read", host_file_read, s);
	bd_vm_register(s->vm, "__bd_file_remove", host_file_remove, s);
	bd_vm_register(s->vm, "__bd_file_list", host_file_list, s);
	bd_vm_eval(s->vm, bootstrap_lua);   /* no-op on the null backend */
	if (!s->profile || !prop_bool(s->profile, "unsafe_scripts", 0))
		bd_vm_eval(s->vm, sandbox_lua); /* default-restricted environment */
}

/* Fire the Lua on.* hooks for an event (no-op if scripting is disabled or the
 * dispatcher is absent). Always passes three string args; unused ones empty. */
/* Ceiling on synchronous hook cascades (e.g. an on.event handler raising
 * another event), so a runaway script loop cannot hang the client. */
#define BD_MAX_DISPATCH_DEPTH 50

static void
dispatch_hook(bd_session *s, const char *kind, const char *a, const char *b)
{
	if (s->dispatch_depth >= BD_MAX_DISPATCH_DEPTH) {
		if (s->log) {           /* record the cap once, where it trips */
			bd_log_rec r;
			log_fill(s, &r, BD_LOG_ERROR);
			r.where = "dispatch";
			r.message = "hook recursion limit reached";
			bd_log_write(s->log, &r);
		}
		return;
	}
	s->dispatch_depth++;
	bd_vm_call(s->vm, "__bd_dispatch", "sss", kind, a ? a : "", b ? b : "");
	s->dispatch_depth--;
}

/* Engine timer-fire callback: run the on.timer hooks with the timer name. */
static void
timer_fired(const char *name, void *ctx)
{
	dispatch_hook((bd_session *)ctx, "timer", name, NULL);
}

/* Engine event callback: run the on.event[name] hook with the event arg. */
static void
event_fired(const char *name, const char *arg, void *ctx)
{
	dispatch_hook((bd_session *)ctx, "event", name, arg);
}

/* Copy `src` to `dst` (cap incl. NUL) dropping ANSI/VT escape sequences, so
 * triggers match on plain text. Handles CSI (ESC [ ... final) and a lone ESC +
 * next byte; other control bytes except tab are dropped. */
static void
strip_ansi(const char *src, size_t len, char *dst, size_t cap)
{
	size_t i, o = 0;
	for (i = 0; i < len && o + 1 < cap; i++) {
		unsigned char ch = (unsigned char)src[i];
		if (ch == 0x1b) {               /* ESC */
			if (i + 1 < len && src[i + 1] == '[') {
				i += 2;
				while (i < len && (src[i] < 0x40 || src[i] > 0x7e))
					i++;            /* params/intermediates */
				/* i now at the final byte; loop's i++ skips it */
			} else {
				i++;                    /* ESC + one byte */
			}
			continue;
		}
		if (ch < 0x20 && ch != '\t')
			continue;               /* drop other control bytes */
		dst[o++] = (char)ch;
	}
	dst[o] = '\0';
}

/* Emit a run of display bytes to the front-end as a DATA event. */
static void
emit_data(bd_session *s, const char *bytes, int len)
{
	bd_session_event ev = { 0 };
	if (len <= 0)
		return;
	ev.kind = BD_SESSION_DATA;
	ev.data = bytes;
	ev.len = len;
	emit(s, &ev);
}

/*
 * Retire the buffered line: run it through the action triggers and on.line
 * hooks, log it, then reconcile the display. The line's content (and any
 * trailing '\r') was already streamed live; here we emit only the line ending,
 * rewritten if a #gag / #substitute / #highlight applies. A gag erases the
 * streamed row (CR + clear-line) and writes no newline; a substitution or
 * highlight erases and rewrites the line; an unmatched line just gets its '\n'
 * (byte-identical to raw passthrough). Limitation: the erase clears one row, so
 * gagging a line that wrapped across rows leaves remnants.
 */
static void
retire_line(bd_session *s)
{
	char plain[4096];
	bd_line_edit edit;
	size_t n = s->line_len;

	if (n > 0 && s->linebuf[n - 1] == '\r')
		n--;
	strip_ansi(s->linebuf, n, plain, sizeof plain);
	bd_triggers_line(s->trig, plain);       /* #action triggers */
	dispatch_hook(s, "line", plain, NULL);  /* on.line hooks */
	bd_triggers_rewrite(s->trig, plain, &edit);

	if (s->log) {                           /* recv record (raw + stripped) */
		char rawz[4096];
		size_t rl = n < sizeof rawz - 1 ? n : sizeof rawz - 1;
		bd_log_rec r;
		memcpy(rawz, s->linebuf, rl);
		rawz[rl] = '\0';
		log_fill(s, &r, BD_LOG_RECV);
		r.raw = rawz;
		r.text = plain;
		r.suppressed = edit.gag;
		bd_log_write(s->log, &r);
	}

	if (edit.gag) {
		emit_data(s, "\r\033[2K", 5);   /* erase the streamed line */
	} else if (edit.changed) {
		emit_data(s, "\r\033[2K", 5);
		emit_data(s, edit.text, (int)strlen(edit.text));
		emit_data(s, "\r\n", 2);
	} else {
		emit_data(s, "\n", 1);          /* normal: just terminate the line */
	}
	s->line_len = 0;
}

/*
 * Assemble incoming bytes into lines. Bytes are streamed live to the display
 * (so prompts and partial lines appear immediately) and buffered for trigger
 * matching; each '\n' retires a line (see retire_line). The unterminated
 * remainder stays buffered and is also the pending prompt text.
 */
static void
feed_lines(bd_session *s, const char *data, int len)
{
	int i, run = 0;
	for (i = 0; i < len; i++) {
		char c = data[i];
		if (c == '\n') {
			emit_data(s, data + run, i - run);  /* stream the line live */
			retire_line(s);                     /* triggers/rewrite/'\n' */
			run = i + 1;
			continue;
		}
		/* buffer the byte for trigger matching (it is streamed in a run) */
		if (s->line_len + 1 > s->line_cap) {
			size_t nc = s->line_cap ? s->line_cap * 2 : 256;
			char *nb = realloc(s->linebuf, nc);
			if (!nb)
				continue;       /* drop the byte rather than grow */
			s->linebuf = nb;
			s->line_cap = nc;
		}
		s->linebuf[s->line_len++] = c;
	}
	if (len > run)                          /* stream the unterminated tail */
		emit_data(s, data + run, len - run);
}

/* ---- MXP (parser callbacks fire from bd_mxp_feed, on the UI thread) ---- */

/* Cleaned display text from the MXP parser feeds the normal line pipeline. */
static void
mxp_text(const char *bytes, size_t len, void *arg)
{
	feed_lines((bd_session *)arg, bytes, (int)len);
}

/* An MXP tag: route to on.mxp[name], the mxp triggers, and the log. */
static void
mxp_tag(const char *name, const char *attrs, int closing, void *arg)
{
	bd_session *s = arg;
	bd_vm_call(s->vm, "__bd_mxp", "ssb", name, attrs, closing);
	if (!closing)                           /* triggers fire on open tags */
		bd_triggers_mxp(s->trig, name, attrs);
	if (s->log) {
		bd_log_rec r;
		log_fill(s, &r, BD_LOG_MXP);
		r.tag = name;
		r.attrs = attrs;
		bd_log_write(s->log, &r);
	}
}

/* ---- bd_net callbacks (fire on the UI thread during drain) ---- */

static void
net_data(const char *data, int len, void *arg)
{
	bd_session *s = arg;
	/* When MXP is active the byte stream is parsed for tags first; its cleaned
	 * text flows on into feed_lines. Otherwise feed_lines streams bytes to the
	 * display run-by-run and retires each completed line through the triggers /
	 * rewriting verbs before the line ending is emitted. */
	if (s->mxp_active && s->mxp)
		bd_mxp_feed(s->mxp, (const unsigned char *)data, (size_t)len);
	else
		feed_lines(s, data, len);
}

/* MXP became active/inactive (telnet option 91). */
static void
net_mxp(int active, void *arg)
{
	bd_session *s = arg;
	s->mxp_active = active;
	if (active && s->mxp)
		bd_mxp_reset(s->mxp);
}

static void
net_state(bd_net_state state, const char *msg, void *arg)
{
	bd_session *s = arg;
	bd_session_event ev = { 0 };
	ev.kind = BD_SESSION_STATE;
	ev.state = state;
	ev.detail = msg;
	emit(s, &ev);

	if (state == BD_NET_CONNECTED) {
		dispatch_hook(s, "connect", NULL, NULL);
		if (s->log) {
			bd_log_rec r;
			log_fill(s, &r, BD_LOG_CONNECT);
			r.host = bd_profile_get(s->profile, "host");
			r.port = atoi(bd_profile_get(s->profile, "port") ?
			    bd_profile_get(s->profile, "port") : "0");
			r.tls = prop_bool(s->profile, "tls", 0);
			bd_log_write(s->log, &r);
		}
	} else if (state == BD_NET_CLOSED || state == BD_NET_ERROR) {
		dispatch_hook(s, "disconnect", NULL, NULL);
		if (s->log) {
			bd_log_rec r;
			log_fill(s, &r, BD_LOG_DISCONNECT);
			r.reason = msg;
			bd_log_write(s->log, &r);
		}
		save_vars(s);           /* persist any var changes this session */
	}
}

static void
net_echo(int suppress, void *arg)
{
	bd_session *s = arg;
	bd_session_event ev = { 0 };
	ev.kind = BD_SESSION_ECHO;
	ev.echo_suppress = suppress;
	emit(s, &ev);
}

static void
net_prompt(void *arg)
{
	bd_session *s = arg;
	bd_session_event ev = { 0 };
	ev.kind = BD_SESSION_PROMPT;
	emit(s, &ev);

	/* the buffered, unterminated text is the prompt: match it (ANSI
	 * stripped) against prompt triggers, then consume it so it does not
	 * merge with the next line */
	if (s->line_len > 0) {
		char plain[4096];
		strip_ansi(s->linebuf, s->line_len, plain, sizeof plain);
		bd_triggers_prompt(s->trig, plain);
		dispatch_hook(s, "prompt", plain, NULL);
		s->line_len = 0;
	}
}

static void
net_package(int proto, const char *name, const char *json, void *arg)
{
	bd_session *s = arg;
	bd_session_event ev = { 0 };
	ev.kind = BD_SESSION_PACKAGE;
	ev.proto = proto;
	ev.name = name;
	ev.json = json;
	emit(s, &ev);

	/* route GMCP/MSDP packages to gmcp-type triggers and on.gmcp hooks */
	bd_triggers_gmcp(s->trig, name, json);
	dispatch_hook(s, "gmcp", name, json);
	if (s->log) {
		bd_log_rec r;
		log_fill(s, &r, BD_LOG_GMCP);
		r.package = name;
		r.data = json;          /* already-parsed JSON text */
		bd_log_write(s->log, &r);
	}
}

/* ---- API ---- */

bd_session *
bd_session_new(const bd_profile *profile)
{
	bd_session *s = calloc(1, sizeof *s);
	if (!s)
		return NULL;
	s->profile = profile;
	s->cols = 80;
	s->rows = 24;
	gen_session_id(s->session_id, sizeof s->session_id);
	s->net = bd_net_new(net_data, net_state, s);
	s->vm = bd_vm_new(&bd_vm_lua);           /* Lua 5.4 + LPeg */
	s->trig = bd_triggers_new(s->vm, trigger_send, s);
	if (s->trig) {
		bd_triggers_set_timer_cb(s->trig, timer_fired, s);
		bd_triggers_set_event_cb(s->trig, event_fired, s);
	}
	if (!s->net || !s->vm || !s->trig) {
		bd_triggers_free(s->trig);
		bd_vm_free(s->vm);
		bd_net_free(s->net);
		free(s);
		return NULL;
	}
	install_script_api(s);
	{
		bd_mxp_cb mc = { mxp_text, mxp_tag, s };
		s->mxp = bd_mxp_new(&mc);
	}
	bd_net_set_echo_cb(s->net, net_echo);
	bd_net_set_prompt_cb(s->net, net_prompt);
	bd_net_set_package_cb(s->net, net_package);
	bd_net_set_mxp_cb(s->net, net_mxp);
	return s;
}

void
bd_session_free(bd_session *s)
{
	if (!s)
		return;
	save_vars(s);                   /* while the vm is still alive */
	/* reverse of build order: net first (stops callbacks), then the engine
	 * it fed, then the vm that engine used */
	bd_net_free(s->net);
	bd_triggers_free(s->trig);
	bd_vm_free(s->vm);
	bd_log_free(s->log);            /* flushes and closes the sink files */
	bd_mxp_free(s->mxp);
	free(s->data_dir);
	free(s->linebuf);
	free(s->scratch_rd);
	free(s->utrig);
	free(s);
}

void
bd_session_on_event(bd_session *s, bd_session_event_fn fn, void *userdata)
{
	if (!s)
		return;
	s->on_event = fn;
	s->userdata = userdata;
}

int
bd_session_connect(bd_session *s)
{
	const char *host, *port, *ttype;
	int tls;

	if (!s || !s->profile)
		return -1;
	host = bd_profile_get(s->profile, "host");
	port = bd_profile_get(s->profile, "port");
	if (!host || !*host || !port || !*port)
		return -1;
	tls = prop_bool(s->profile, "tls", 0);

	bd_net_set_autoreconnect(s->net, prop_bool(s->profile, "autoreconnect", 1));
	ttype = bd_profile_get(s->profile, "termtype");
	bd_net_set_termtype(s->net, ttype ? ttype : "birdie/0.0");
	bd_net_set_winsize(s->net, s->cols, s->rows);

	s->mxp_active = 0;                       /* renegotiated each connection */
	bd_mxp_reset(s->mxp);

	return bd_net_connect(s->net, host, port, tls);
}

void
bd_session_disconnect(bd_session *s)
{
	if (s)
		bd_net_close(s->net);
}

bd_net_state
bd_session_state(const bd_session *s)
{
	return s ? bd_net_state_get(s->net) : BD_NET_IDLE;
}

int
bd_session_send_line(bd_session *s, const char *utf8)
{
	int n;

	if (!s || !utf8)
		return -1;
	/* an alias may consume the input and emit its own commands (logged via
	 * trigger_send); only an un-aliased line is logged here */
	if (bd_triggers_input(s->trig, utf8))
		return 0;
	n = bd_net_send(s->net, utf8, (int)strlen(utf8));
	if (n < 0)
		return -1;
	bd_net_send(s->net, "\r\n", 2);
	log_send(s, utf8);
	return n;
}

int
bd_session_send_raw(bd_session *s, const void *bytes, size_t n)
{
	if (!s)
		return -1;
	return bd_net_send(s->net, bytes, (int)n);
}

void
bd_session_set_winsize(bd_session *s, int cols, int rows)
{
	if (!s || cols <= 0 || rows <= 0)
		return;
	s->cols = cols;
	s->rows = rows;
	bd_net_set_winsize(s->net, cols, rows);
}

void
bd_session_set_data_dir(bd_session *s, const char *dir)
{
	if (!s)
		return;
	free(s->data_dir);
	s->data_dir = (dir && *dir) ? strdup(dir) : NULL;
	load_vars(s);           /* pull the saved var table into the VM */

	/* (re)build the default log sinks under <data_dir>/logs: a full NDJSON
	 * log (source of truth) and a human-readable plaintext traffic log, each
	 * hour-bucketed per profile (doc/logging.md) */
	bd_log_free(s->log);
	s->log = NULL;
	if (s->data_dir) {
		char root[1024];
		const char *tmpl =
		    "{root}/{year}/{mud}/{character}/{year}-{month}-{day}-{hour}00.{ext}";
		snprintf(root, sizeof root, "%s/logs", s->data_dir);
		s->log = bd_log_new();
		if (s->log) {
			bd_log_add_file(s->log, BD_LOGF_ALL, BD_LOGFMT_NDJSON,
			    root, tmpl);
			bd_log_add_file(s->log, BD_LOGF_TRAFFIC,
			    BD_LOGFMT_PLAINTEXT, root, tmpl);
		}
	}
}

int
bd_session_load_profile_script(bd_session *s)
{
	char path[1024];
	FILE *f;
	long sz;
	char *buf;
	size_t got;
	int rc;

	if (!s || profile_file_path(s, "triggers.lua", path, sizeof path) != 0)
		return 0;
	f = fopen(path, "rb");
	if (!f)
		return 0;                       /* no script for this profile */
	fseek(f, 0, SEEK_END);
	sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (sz <= 0) {
		fclose(f);
		return 0;
	}
	buf = malloc((size_t)sz + 1);
	if (!buf) {
		fclose(f);
		return 0;
	}
	got = fread(buf, 1, (size_t)sz, f);
	fclose(f);
	buf[got] = '\0';
	rc = bd_vm_eval(s->vm, buf);             /* runs in the sandbox */
	free(buf);
	return rc == 0 ? 1 : -1;
}

/* Find a user trigger by identity (type + pattern + class). A NULL/"" cls
 * matches an entry whose class is also empty. Returns its index or -1. */
static int
user_trig_find(bd_session *s, bd_trigger_type type, const char *pattern,
               const char *cls)
{
	int i;
	const char *c = (cls && *cls) ? cls : "";
	for (i = 0; i < s->nutrig; i++) {
		if (s->utrig[i].type == type &&
		    strcmp(s->utrig[i].pattern, pattern) == 0 &&
		    strcmp(s->utrig[i].cls, c) == 0)
			return i;
	}
	return -1;
}

int
bd_session_user_trigger_add(bd_session *s, bd_trigger_type type,
                            const char *pattern, const char *body,
                            const char *cls, int priority, int stop)
{
	struct user_trig *u;
	int idx;

	if (!s || !s->trig || !pattern || !*pattern)
		return -1;

	/* install (or reinstall) in the live engine: drop any prior same-key
	 * trigger first so a re-add updates rather than duplicates. */
	bd_trigger_remove_pattern(s->trig, type, pattern,
	                          (cls && *cls) ? cls : NULL);
	if (bd_trigger_add(s->trig, type, pattern, body,
	                   (cls && *cls) ? cls : NULL, priority, stop) < 0)
		return -1;

	idx = user_trig_find(s, type, pattern, cls);
	if (idx < 0) {
		if (s->nutrig >= s->ucap) {
			int cap = s->ucap ? s->ucap * 2 : 16;
			struct user_trig *p = realloc(s->utrig,
			                              (size_t)cap * sizeof *p);
			if (!p)
				return -1;
			s->utrig = p;
			s->ucap = cap;
		}
		idx = s->nutrig++;
	}
	u = &s->utrig[idx];
	u->type = type;
	snprintf(u->pattern, sizeof u->pattern, "%s", pattern);
	snprintf(u->body, sizeof u->body, "%s", body ? body : "");
	snprintf(u->cls, sizeof u->cls, "%s", (cls && *cls) ? cls : "");
	u->priority = priority;
	u->stop = stop;
	return 0;
}

int
bd_session_user_trigger_remove(bd_session *s, bd_trigger_type type,
                               const char *pattern, const char *cls)
{
	int idx;

	if (!s || !s->trig || !pattern)
		return -1;

	bd_trigger_remove_pattern(s->trig, type, pattern,
	                          (cls && *cls) ? cls : NULL);
	idx = user_trig_find(s, type, pattern, cls);
	if (idx >= 0) {
		memmove(&s->utrig[idx], &s->utrig[idx + 1],
		        (size_t)(s->nutrig - idx - 1) * sizeof *s->utrig);
		s->nutrig--;
	}
	return 0;
}

int
bd_session_user_trigger_count(const bd_session *s)
{
	return s ? s->nutrig : 0;
}

int
bd_session_save_triggers(bd_session *s)
{
	char path[1024], dir[1024], *slash;
	bd_csv_w *w;
	const char *csv;
	size_t len;
	FILE *f;
	int i;

	if (!s || profile_file_path(s, "triggers.csv", path, sizeof path) != 0)
		return -1;

	w = bd_csv_w_new();
	if (!w)
		return -1;
	bd_csv_w_field(w, "type");
	bd_csv_w_field(w, "pattern");
	bd_csv_w_field(w, "body");
	bd_csv_w_field(w, "class");
	bd_csv_w_field(w, "priority");
	bd_csv_w_field(w, "stop");
	bd_csv_w_endrow(w);
	for (i = 0; i < s->nutrig; i++) {
		char num[16];
		const struct user_trig *u = &s->utrig[i];
		bd_csv_w_field(w, trig_type_name(u->type));
		bd_csv_w_field(w, u->pattern);
		bd_csv_w_field(w, u->body);
		bd_csv_w_field(w, u->cls);
		snprintf(num, sizeof num, "%d", u->priority);
		bd_csv_w_field(w, num);
		snprintf(num, sizeof num, "%d", u->stop ? 1 : 0);
		bd_csv_w_field(w, num);
		bd_csv_w_endrow(w);
	}
	csv = bd_csv_w_str(w, &len);

	snprintf(dir, sizeof dir, "%s", path);
	slash = strrchr(dir, '/');
	if (slash) {
		*slash = '\0';
		mkdir_p(dir);
	}
	f = fopen(path, "wb");
	if (!f) {
		bd_csv_w_free(w);
		return -1;
	}
	fwrite(csv, 1, len, f);
	fclose(f);
	bd_csv_w_free(w);
	return 0;
}

/* Column index of `name` in the header row, or -1. */
static int
csv_col(const bd_csv *c, const char *name)
{
	int n = bd_csv_cols(c, 0), i;
	for (i = 0; i < n; i++) {
		const char *h = bd_csv_get(c, 0, i);
		if (h && strcmp(h, name) == 0)
			return i;
	}
	return -1;
}

int
bd_session_load_triggers(bd_session *s)
{
	char path[1024];
	FILE *f;
	long sz;
	char *buf;
	size_t got;
	bd_csv *c;
	int col_type, col_pat, col_body, col_cls, col_prio, col_stop;
	int rows, r, loaded = 0;

	if (!s || profile_file_path(s, "triggers.csv", path, sizeof path) != 0)
		return 0;
	f = fopen(path, "rb");
	if (!f)
		return 0;                       /* no user triggers for this profile */
	fseek(f, 0, SEEK_END);
	sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (sz <= 0) {
		fclose(f);
		return 0;
	}
	buf = malloc((size_t)sz);
	if (!buf) {
		fclose(f);
		return -1;
	}
	got = fread(buf, 1, (size_t)sz, f);
	fclose(f);

	c = bd_csv_parse(buf, got);
	free(buf);
	if (!c)
		return -1;

	col_type = csv_col(c, "type");
	col_pat = csv_col(c, "pattern");
	col_body = csv_col(c, "body");
	col_cls = csv_col(c, "class");
	col_prio = csv_col(c, "priority");
	col_stop = csv_col(c, "stop");
	if (col_type < 0 || col_pat < 0) {      /* not our schema */
		bd_csv_free(c);
		return -1;
	}

	rows = bd_csv_rows(c);
	for (r = 1; r < rows; r++) {
		bd_trigger_type type;
		const char *pat, *body, *cls, *prio, *stop;
		if (trig_type_parse(bd_csv_get(c, r, col_type), &type) != 0)
			continue;
		pat = bd_csv_get(c, r, col_pat);
		if (!pat || !*pat)
			continue;
		body = col_body >= 0 ? bd_csv_get(c, r, col_body) : NULL;
		cls = col_cls >= 0 ? bd_csv_get(c, r, col_cls) : NULL;
		prio = col_prio >= 0 ? bd_csv_get(c, r, col_prio) : NULL;
		stop = col_stop >= 0 ? bd_csv_get(c, r, col_stop) : NULL;
		if (bd_session_user_trigger_add(s, type, pat, body, cls,
		                                prio ? atoi(prio) : -1,
		                                stop ? atoi(stop) : 0) == 0)
			loaded++;
	}
	bd_csv_free(c);
	return loaded;
}

bd_triggers *
bd_session_triggers(bd_session *s)
{
	return s ? s->trig : NULL;
}

bd_vm *
bd_session_vm(bd_session *s)
{
	return s ? s->vm : NULL;
}

/* Re-emit one replayed line as a DATA event (no triggers, no line assembly). */
static void
replay_emit(const char *raw, size_t len, void *ctx)
{
	bd_session *s = ctx;
	bd_session_event ev = { 0 };
	ev.kind = BD_SESSION_DATA;
	ev.data = raw;
	ev.len = (int)len;
	ev.replay = 1;
	emit(s, &ev);
	ev.data = "\r\n";       /* the logged raw line has no trailing newline */
	ev.len = 2;
	emit(s, &ev);
}

int
bd_session_replay(bd_session *s, int max)
{
	char root[1024];
	const char *mud, *character;

	if (!s || !s->data_dir || max <= 0)
		return -1;
	mud = s->profile ? bd_profile_get(s->profile, "name") : NULL;
	character = s->profile ? bd_profile_get(s->profile, "character") : NULL;
	snprintf(root, sizeof root, "%s/logs", s->data_dir);
	return bd_replay_recv(root, mud, character, max, replay_emit, s);
}

void
bd_session_drain(bd_session *s)
{
	double now;
	if (!s)
		return;
	now = mono_ms();
	bd_triggers_set_now(s->trig, now);      /* clock for chain timeouts */
	bd_net_poll(s->net);                    /* network -> events/triggers */
	bd_triggers_run_timers(s->trig, now);   /* #tick interval timers */
	bd_vm_call(s->vm, "__bd_poll_expr", ""); /* expression watches (on change) */
}
