/*
 * bd_vm -- scripting VM abstraction + null and recording backends.
 * See bd_vm.h and doc/triggers.md. The Lua backend is bd_vm_lua.c.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#include "bd_vm.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ARGS 16

struct bd_vm {
	const bd_vm_backend *be;
	void *impl;
};

/* ---- public wrapper ---- */

bd_vm *
bd_vm_new(const bd_vm_backend *backend)
{
	bd_vm *vm = calloc(1, sizeof *vm);
	if (!vm)
		return NULL;
	vm->be = backend ? backend : &bd_vm_null;
	vm->impl = vm->be->create ? vm->be->create() : NULL;
	return vm;
}

void
bd_vm_free(bd_vm *vm)
{
	if (!vm)
		return;
	if (vm->be->destroy)
		vm->be->destroy(vm->impl);
	free(vm);
}

int
bd_vm_eval(bd_vm *vm, const char *source)
{
	if (!vm || !source)
		return -1;
	return vm->be->eval(vm->impl, source);
}

/* Marshal a printf-ish arg spec into a bd_vm_val array. */
static int
build_args(const char *spec, va_list ap, bd_vm_val *argv, int max)
{
	int n = 0;
	if (!spec)
		return 0;
	for (; *spec && n < max; spec++) {
		switch (*spec) {
		case 's': argv[n++] = bd_vm_str(va_arg(ap, const char *)); break;
		case 'i': argv[n++] = bd_vm_num((double)va_arg(ap, int)); break;
		case 'b': argv[n++] = bd_vm_bool(va_arg(ap, int)); break;
		case 'd': argv[n++] = bd_vm_num(va_arg(ap, double)); break;
		default:  return -1;     /* unknown spec letter */
		}
	}
	return n;
}

int
bd_vm_call(bd_vm *vm, const char *name, const char *spec, ...)
{
	bd_vm_val argv[MAX_ARGS];
	int argc;
	va_list ap;

	if (!vm || !name)
		return -1;
	va_start(ap, spec);
	argc = build_args(spec, ap, argv, MAX_ARGS);
	va_end(ap);
	if (argc < 0)
		return -1;
	return vm->be->call(vm->impl, name, argc, argv);
}

void
bd_vm_register(bd_vm *vm, const char *name, bd_host_fn fn, void *ud)
{
	if (vm && name && fn && vm->be->reg)
		vm->be->reg(vm->impl, name, fn, ud, vm);
}

const char *
bd_vm_error(bd_vm *vm)
{
	if (!vm)
		return "no vm";
	return vm->be->error ? vm->be->error(vm->impl) : "";
}

/* ---- null backend (scripting disabled) ---- */

static void *null_create(void) { return NULL; }
static void  null_destroy(void *impl) { (void)impl; }
static int   null_eval(void *impl, const char *s) { (void)impl; (void)s; return -1; }
static int   null_call(void *impl, const char *n, int argc, const bd_vm_val *argv)
{ (void)impl; (void)n; (void)argc; (void)argv; return -1; }
static void  null_reg(void *impl, const char *n, bd_host_fn fn, void *ud, bd_vm *vm)
{ (void)impl; (void)n; (void)fn; (void)ud; (void)vm; }
static const char *null_error(void *impl) { (void)impl; return "scripting disabled"; }

const bd_vm_backend bd_vm_null = {
	.name = "null",
	.create = null_create,
	.destroy = null_destroy,
	.eval = null_eval,
	.call = null_call,
	.reg = null_reg,
	.error = null_error,
};

/* ---- recording backend (tests) ---- */
/*
 * Records every eval'd source and every call (name + a copy of its args), so a
 * test can assert what the trigger engine asked the VM to do. It does not run
 * script logic, so registered host functions are stored but never invoked here.
 */

struct rec_call {
	char *name;
	int argc;
	bd_vm_val argv[MAX_ARGS];
	char *strs[MAX_ARGS];   /* owned copies backing BD_VM_STR argv entries */
};

struct rec {
	char **evals;
	int neval, eval_cap;
	struct rec_call *calls;
	int ncall, call_cap;
};

static void *
rec_create(void)
{
	return calloc(1, sizeof(struct rec));
}

static void
rec_clear_impl(struct rec *r)
{
	int i, j;
	for (i = 0; i < r->neval; i++)
		free(r->evals[i]);
	r->neval = 0;
	for (i = 0; i < r->ncall; i++) {
		free(r->calls[i].name);
		for (j = 0; j < r->calls[i].argc; j++)
			free(r->calls[i].strs[j]);
	}
	r->ncall = 0;
}

static void
rec_destroy(void *impl)
{
	struct rec *r = impl;
	if (!r)
		return;
	rec_clear_impl(r);
	free(r->evals);
	free(r->calls);
	free(r);
}

static int
rec_eval(void *impl, const char *src)
{
	struct rec *r = impl;
	if (r->neval == r->eval_cap) {
		int nc = r->eval_cap ? r->eval_cap * 2 : 8;
		char **t = realloc(r->evals, (size_t)nc * sizeof *t);
		if (!t)
			return -1;
		r->evals = t;
		r->eval_cap = nc;
	}
	r->evals[r->neval] = strdup(src);
	if (!r->evals[r->neval])
		return -1;
	r->neval++;
	return 0;
}

static int
rec_call(void *impl, const char *name, int argc, const bd_vm_val *argv)
{
	struct rec *r = impl;
	struct rec_call *c;
	int j;

	if (r->ncall == r->call_cap) {
		int nc = r->call_cap ? r->call_cap * 2 : 8;
		struct rec_call *t = realloc(r->calls, (size_t)nc * sizeof *t);
		if (!t)
			return -1;
		r->calls = t;
		r->call_cap = nc;
	}
	c = &r->calls[r->ncall];
	memset(c, 0, sizeof *c);
	c->name = strdup(name);
	c->argc = argc < MAX_ARGS ? argc : MAX_ARGS;
	for (j = 0; j < c->argc; j++) {
		c->argv[j] = argv[j];
		if (argv[j].type == BD_VM_STR && argv[j].u.s) {
			c->strs[j] = strdup(argv[j].u.s);
			c->argv[j].u.s = c->strs[j];   /* point at our copy */
		}
	}
	r->ncall++;
	return 0;
}

static void
rec_reg(void *impl, const char *n, bd_host_fn fn, void *ud, bd_vm *vm)
{
	(void)impl; (void)n; (void)fn; (void)ud; (void)vm;   /* stored nowhere */
}

static const char *
rec_error(void *impl) { (void)impl; return ""; }

const bd_vm_backend bd_vm_recording = {
	.name = "recording",
	.create = rec_create,
	.destroy = rec_destroy,
	.eval = rec_eval,
	.call = rec_call,
	.reg = rec_reg,
	.error = rec_error,
};

/* ---- recording inspection ---- */
/* These reach into the impl; valid only for a bd_vm on bd_vm_recording. */

static struct rec *
rec_of(bd_vm *vm)
{
	if (!vm || vm->be != &bd_vm_recording)
		return NULL;
	return vm->impl;
}

int
bd_vm_rec_eval_count(bd_vm *vm)
{
	struct rec *r = rec_of(vm);
	return r ? r->neval : 0;
}

const char *
bd_vm_rec_eval(bd_vm *vm, int i)
{
	struct rec *r = rec_of(vm);
	return (r && i >= 0 && i < r->neval) ? r->evals[i] : NULL;
}

int
bd_vm_rec_call_count(bd_vm *vm)
{
	struct rec *r = rec_of(vm);
	return r ? r->ncall : 0;
}

const char *
bd_vm_rec_call_name(bd_vm *vm, int i)
{
	struct rec *r = rec_of(vm);
	return (r && i >= 0 && i < r->ncall) ? r->calls[i].name : NULL;
}

int
bd_vm_rec_call_argc(bd_vm *vm, int i)
{
	struct rec *r = rec_of(vm);
	return (r && i >= 0 && i < r->ncall) ? r->calls[i].argc : 0;
}

bd_vm_val
bd_vm_rec_call_arg(bd_vm *vm, int i, int j)
{
	struct rec *r = rec_of(vm);
	if (!r || i < 0 || i >= r->ncall || j < 0 || j >= r->calls[i].argc)
		return bd_vm_nil();
	return r->calls[i].argv[j];
}

void
bd_vm_rec_clear(bd_vm *vm)
{
	struct rec *r = rec_of(vm);
	if (r)
		rec_clear_impl(r);
}
