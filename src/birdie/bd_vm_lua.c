/*
 * bd_vm_lua -- the Lua 5.4 + LPeg backend for bd_vm (doc/triggers.md).
 *
 * Maps the bd_vm_backend vtable onto a lua_State: eval runs a chunk, call
 * invokes a global function with marshalled args, and register exposes a host
 * C function to scripts (a closure trampoline converts the Lua stack to/from
 * bd_vm_val). LPeg is linked in and registered at startup. Only built/linked
 * when Lua is vendored (src/thirdparty/lua, src/thirdparty/lpeg).
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#include "bd_vm.h"

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define LVM_MAX_ARGS 16   /* matches bd_vm.c's call marshalling cap */

/* LPeg's loader (declared here so we need no LPeg header). */
extern int luaopen_lpeg(lua_State *L);

struct lvm {
	lua_State *L;
	char err[256];
};

static void
push_val(lua_State *L, const bd_vm_val *a)
{
	switch (a->type) {
	case BD_VM_BOOL: lua_pushboolean(L, a->u.b); break;
	case BD_VM_NUM:  lua_pushnumber(L, a->u.n); break;
	case BD_VM_STR:  lua_pushstring(L, a->u.s ? a->u.s : ""); break;
	default:         lua_pushnil(L); break;
	}
}

/* ---- backend hooks ---- */

static void *
lvm_create(void)
{
	struct lvm *v = calloc(1, sizeof *v);
	if (!v)
		return NULL;
	v->L = luaL_newstate();
	if (!v->L) {
		free(v);
		return NULL;
	}
	luaL_openlibs(v->L);
	luaL_requiref(v->L, "lpeg", luaopen_lpeg, 1);   /* global + package */
	lua_pop(v->L, 1);
	return v;
}

static void
lvm_destroy(void *impl)
{
	struct lvm *v = impl;
	if (!v)
		return;
	if (v->L)
		lua_close(v->L);
	free(v);
}

static int
lvm_eval(void *impl, const char *src)
{
	struct lvm *v = impl;
	if (luaL_loadstring(v->L, src) != LUA_OK ||
	    lua_pcall(v->L, 0, 0, 0) != LUA_OK) {
		snprintf(v->err, sizeof v->err, "%s",
		    lua_tostring(v->L, -1) ? lua_tostring(v->L, -1) : "error");
		lua_pop(v->L, 1);
		return -1;
	}
	return 0;
}

static int
lvm_call(void *impl, const char *name, int argc, const bd_vm_val *argv)
{
	struct lvm *v = impl;
	int i;

	if (lua_getglobal(v->L, name) != LUA_TFUNCTION) {
		lua_pop(v->L, 1);
		snprintf(v->err, sizeof v->err, "not a function: %s", name);
		return -1;
	}
	for (i = 0; i < argc; i++)
		push_val(v->L, &argv[i]);
	if (lua_pcall(v->L, argc, 0, 0) != LUA_OK) {
		snprintf(v->err, sizeof v->err, "%s",
		    lua_tostring(v->L, -1) ? lua_tostring(v->L, -1) : "error");
		lua_pop(v->L, 1);
		return -1;
	}
	return 0;
}

/* A registered host function: stored as full userdata so Lua GCs it with the
 * closure that wraps the trampoline. */
struct hostbox {
	bd_host_fn fn;
	void *ud;
	bd_vm *vm;
};

static int
host_trampoline(lua_State *L)
{
	struct hostbox *b = lua_touserdata(L, lua_upvalueindex(1));
	int argc = lua_gettop(L), i;
	bd_vm_val argv[LVM_MAX_ARGS];
	bd_vm_val ret = bd_vm_nil();

	if (argc > LVM_MAX_ARGS)
		argc = LVM_MAX_ARGS;
	for (i = 0; i < argc; i++) {
		switch (lua_type(L, i + 1)) {
		case LUA_TBOOLEAN: argv[i] = bd_vm_bool(lua_toboolean(L, i + 1)); break;
		case LUA_TNUMBER:  argv[i] = bd_vm_num(lua_tonumber(L, i + 1)); break;
		case LUA_TSTRING:  argv[i] = bd_vm_str(lua_tostring(L, i + 1)); break;
		default:           argv[i] = bd_vm_nil(); break;
		}
	}
	b->fn(b->vm, argc, argv, &ret, b->ud);
	push_val(L, &ret);
	return 1;
}

static void
lvm_reg(void *impl, const char *name, bd_host_fn fn, void *ud, bd_vm *vm)
{
	struct lvm *v = impl;
	struct hostbox *b = lua_newuserdatauv(v->L, sizeof *b, 0);
	b->fn = fn;
	b->ud = ud;
	b->vm = vm;
	lua_pushcclosure(v->L, host_trampoline, 1);   /* userdata is upvalue 1 */
	lua_setglobal(v->L, name);
}

static const char *
lvm_error(void *impl)
{
	struct lvm *v = impl;
	return v ? v->err : "no vm";
}

const bd_vm_backend bd_vm_lua = {
	.name = "lua",
	.create = lvm_create,
	.destroy = lvm_destroy,
	.eval = lvm_eval,
	.call = lvm_call,
	.reg = lvm_reg,
	.error = lvm_error,
};
