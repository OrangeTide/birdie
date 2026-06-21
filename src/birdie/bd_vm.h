#ifndef BD_VM_H
#define BD_VM_H

/*
 * bd_vm -- birdie's scripting VM abstraction (doc/triggers.md).
 *
 * The trigger engine, the log-note hook, and widget event routing talk to this
 * opaque handle, never to a concrete interpreter. The v1.0 backend is Lua 5.4
 * + LPeg; a "null" backend (scripting disabled) and a "recording" backend (for
 * tests) ship too. A fork can drop in another language by implementing the
 * bd_vm_backend vtable without touching anything above this seam.
 *
 * Values crossing the seam are scalars: nil / bool / number / string. That
 * covers the host API (mud.send, log.note, class.enable, ...) and the trigger
 * dispatch (on.line(text), on.gmcp[pkg](json), ...). Structured data (GMCP /
 * MSDP tables) crosses as a JSON string and is decoded on the script side.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#include <stddef.h>

typedef enum bd_vm_type {
	BD_VM_NIL,
	BD_VM_BOOL,
	BD_VM_NUM,
	BD_VM_STR
} bd_vm_type;

/* A value crossing the seam. For BD_VM_STR, `s` is borrowed for the duration
 * of the call unless a backend copies it. */
typedef struct bd_vm_val {
	bd_vm_type type;
	union {
		int         b;
		double      n;
		const char *s;
	} u;
} bd_vm_val;

static inline bd_vm_val bd_vm_nil(void)
{ bd_vm_val v; v.type = BD_VM_NIL; v.u.s = 0; return v; }
static inline bd_vm_val bd_vm_bool(int b)
{ bd_vm_val v; v.type = BD_VM_BOOL; v.u.b = b ? 1 : 0; return v; }
static inline bd_vm_val bd_vm_num(double n)
{ bd_vm_val v; v.type = BD_VM_NUM; v.u.n = n; return v; }
static inline bd_vm_val bd_vm_str(const char *s)
{ bd_vm_val v; v.type = BD_VM_STR; v.u.s = s; return v; }

typedef struct bd_vm bd_vm;

/*
 * A host C function exposed to scripts. It receives the call arguments and may
 * fill one return value (left BD_VM_NIL if none). Returns 0 on success, -1 on
 * error. `ud` is the pointer registered with it.
 */
typedef int (*bd_host_fn)(bd_vm *vm, int argc, const bd_vm_val *argv,
                          bd_vm_val *ret, void *ud);

/*
 * A scripting backend. create() returns an implementation handle stored in the
 * bd_vm; the other hooks receive it. All hooks are required except where noted.
 */
typedef struct bd_vm_backend {
	const char *name;
	void *(*create)(void);
	void  (*destroy)(void *impl);
	/* run source; return 0 on success, -1 on error (message via error()) */
	int   (*eval)(void *impl, const char *source);
	/* call global function `name` with argv[argc]; return 0 on success,
	 * -1 on error or if no such function */
	int   (*call)(void *impl, const char *name,
	              int argc, const bd_vm_val *argv);
	/* expose `fn` to scripts as global `name` */
	void  (*reg)(void *impl, const char *name, bd_host_fn fn, void *ud,
	             bd_vm *vm);
	/* last error string (never NULL); valid until the next backend call */
	const char *(*error)(void *impl);
} bd_vm_backend;

/* The three shipped backends. */
extern const bd_vm_backend bd_vm_null;       /* scripting disabled */
extern const bd_vm_backend bd_vm_recording;  /* records eval/call for tests */
/* bd_vm_lua lives in bd_vm_lua.c and is only built when Lua is vendored. */

/* Create a VM on a backend (NULL selects bd_vm_null). */
bd_vm *bd_vm_new(const bd_vm_backend *backend);
void   bd_vm_free(bd_vm *vm);

/* Run a chunk of source. Returns 0 on success, -1 on error. */
int    bd_vm_eval(bd_vm *vm, const char *source);

/* Call a global script function. `spec` is one letter per argument:
 *   's' const char*   'i' int   'd' double   'b' int (bool)
 * e.g. bd_vm_call(vm, "on_line", "s", text). Returns 0 on success, -1 on
 * error or if the function is absent. */
int    bd_vm_call(bd_vm *vm, const char *name, const char *spec, ...);

/* Expose a host function to scripts as global `name`. */
void   bd_vm_register(bd_vm *vm, const char *name, bd_host_fn fn, void *ud);

/* Last error message (never NULL). */
const char *bd_vm_error(bd_vm *vm);

/* ---- recording backend test inspection ----
 * Valid only on a bd_vm created with bd_vm_recording. */

int         bd_vm_rec_eval_count(bd_vm *vm);
const char *bd_vm_rec_eval(bd_vm *vm, int i);     /* i-th eval'd source */
int         bd_vm_rec_call_count(bd_vm *vm);
const char *bd_vm_rec_call_name(bd_vm *vm, int i); /* i-th call's function */
int         bd_vm_rec_call_argc(bd_vm *vm, int i);
/* j-th argument of the i-th recorded call (BD_VM_NIL if out of range) */
bd_vm_val   bd_vm_rec_call_arg(bd_vm *vm, int i, int j);
void        bd_vm_rec_clear(bd_vm *vm);

#endif /* BD_VM_H */
