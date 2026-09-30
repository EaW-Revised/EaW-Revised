#pragma once

// Included by the lparser.c and lcode.c wrappers after sflua_upstream.hpp.
// Every item the compiler appends to a prototype's arrays (an instruction and
// its line, a constant, a child prototype, a local, an upvalue name) is
// charged to the sandbox's memory quota before the array can grow, by the
// logical sizes of docs/lua-sandbox.md (sflua_sandbox.cpp). Each
// luaM_growvector of the two files appends exactly one item.

EAWR_SFLUA_BEGIN
#include "lobject.h"
#include "lmem.h"

void sandbox_before_prototype_item(lua_State* L, const Instruction* code);
void sandbox_before_prototype_item(lua_State* L, const int* lineinfo);
void sandbox_before_prototype_item(lua_State* L, const TObject* constant);
void sandbox_before_prototype_item(lua_State* L, Proto* const* child);
void sandbox_before_prototype_item(lua_State* L, const LocVar* local);
void sandbox_before_prototype_item(lua_State* L, TString* const* upvalue_name);
EAWR_SFLUA_END

#undef luaM_growvector
#define luaM_growvector(L,v,nelems,size,t,limit,e) \
          if ((sandbox_before_prototype_item(L, v), ((nelems)+1) > (size))) \
            ((v)=cast(t *, luaM_growaux(L,v,&(size),sizeof(t),limit,e)))
