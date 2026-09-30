// Upstream Lua 5.0.2 src/lib/lstrlib.c, unmodified, compiled for the authoritative profile.
#include "sflua_metering.hpp"
#include "sflua_upstream.hpp"

EAWR_SFLUA_BEGIN
#include "lstrlib.c"
EAWR_SFLUA_END

// Stable names for persistence (sflua_persist.cpp).
EAWR_SFLUA_BEGIN
const luaL_reg* sflua_string_functions() { return strlib; }
lua_CFunction sflua_gfind_iterator() { return gfind_aux; }
EAWR_SFLUA_END

// Metered pattern matching (docs/lua-sandbox.md, costs). string.find, gfind
// and gsub of the sandbox are str_find, gfind/gfind_aux and str_gsub above
// with lstrlib.c's matcher and add_s, line for line, charging each matcher
// step and each replacement to the sandbox budget before taking it; results
// and error messages are upstream's.
// The helpers that do bounded work (luaI_classend, luaI_singlematch,
// matchbracketclass, check_capture, capture_to_close, push_onecapture,
// push_captures) are lstrlib.c's own.
//
// Units: 1 per pattern item visited; the item's length (1 for a character or
// `.', 2 for a %-class, the whole set for [...]) per character tested against
// it, charged once its end is known (so at most one item scan runs before the
// charge); 1 per character scanned by %b; the capture's length per back
// reference; 1 per position tried and the pattern's length per candidate
// compared by a plain find; per gsub substitution, the replacement string's
// length and the length of each capture it copies, or for a replacement
// function the length of the captures passed to it and of the string it
// returns (Lua 5.0.2 has no table replacement).
EAWR_SFLUA_BEGIN
void sandbox_charge_steps(lua_State* L, ::eawr::script::sflua_metering::StepUnits units);

#define METER(ms, units) sandbox_charge_steps((ms)->L, static_cast<::eawr::script::sflua_metering::StepUnits>(units))

static const char *metered_match (MatchState *ms, const char *s, const char *p);


static const char *metered_matchbalance (MatchState *ms, const char *s,
                                           const char *p) {
  if (*p == 0 || *(p+1) == 0)
    luaL_error(ms->L, "unbalanced pattern");
  if (*s != *p) return NULL;
  else {
    int b = *p;
    int e = *(p+1);
    int cont = 1;
    while (METER(ms, 1), ++s < ms->src_end) {
      if (*s == e) {
        if (--cont == 0) return s+1;
      }
      else if (*s == b) cont++;
    }
  }
  return NULL;  /* string ends out of balance */
}


static const char *metered_max_expand (MatchState *ms, const char *s,
                                         const char *p, const char *ep) {
  sint32 i = 0;  /* counts maximum expand for item */
  while ((s+i)<ms->src_end && (METER(ms, ep-p), luaI_singlematch(uchar(*(s+i)), p, ep)))
    i++;
  /* keeps trying to match with the maximum repetitions */
  while (i>=0) {
    const char *res = metered_match(ms, (s+i), ep+1);
    if (res) return res;
    i--;  /* else didn't match; reduce 1 repetition to try again */
  }
  return NULL;
}


static const char *metered_min_expand (MatchState *ms, const char *s,
                                         const char *p, const char *ep) {
  for (;;) {
    const char *res = metered_match(ms, s, ep+1);
    if (res != NULL)
      return res;
    else if (s<ms->src_end && (METER(ms, ep-p), luaI_singlematch(uchar(*s), p, ep)))
      s++;  /* try with one more repetition */
    else return NULL;
  }
}


static const char *metered_start_capture (MatchState *ms, const char *s,
                                            const char *p, int what) {
  const char *res;
  int level = ms->level;
  if (level >= MAX_CAPTURES) luaL_error(ms->L, "too many captures");
  ms->capture[level].init = s;
  ms->capture[level].len = what;
  ms->level = level+1;
  if ((res=metered_match(ms, s, p)) == NULL)  /* match failed? */
    ms->level--;  /* undo capture */
  return res;
}


static const char *metered_end_capture (MatchState *ms, const char *s,
                                          const char *p) {
  int l = capture_to_close(ms);
  const char *res;
  ms->capture[l].len = s - ms->capture[l].init;  /* close capture */
  if ((res = metered_match(ms, s, p)) == NULL)  /* match failed? */
    ms->capture[l].len = CAP_UNFINISHED;  /* undo capture */
  return res;
}


static const char *metered_match_capture (MatchState *ms, const char *s, int l) {
  size_t len;
  l = check_capture(ms, l);
  len = ms->capture[l].len;
  if ((size_t)(ms->src_end-s) >= len &&
      (METER(ms, len), memcmp(ms->capture[l].init, s, len) == 0))
    return s+len;
  else return NULL;
}


static const char *metered_match (MatchState *ms, const char *s, const char *p) {
  init: /* using goto's to optimize tail recursion */
  METER(ms, 1);
  switch (*p) {
    case '(': {  /* start capture */
      if (*(p+1) == ')')  /* position capture? */
        return metered_start_capture(ms, s, p+2, CAP_POSITION);
      else
        return metered_start_capture(ms, s, p+1, CAP_UNFINISHED);
    }
    case ')': {  /* end capture */
      return metered_end_capture(ms, s, p+1);
    }
    case ESC: {
      switch (*(p+1)) {
        case 'b': {  /* balanced string? */
          s = metered_matchbalance(ms, s, p+2);
          if (s == NULL) return NULL;
          p+=4; goto init;  /* else return match(ms, s, p+4); */
        }
        case 'f': {  /* frontier? */
          const char *ep; char previous;
          p += 2;
          if (*p != '[')
            luaL_error(ms->L, "missing `[' after `%%f' in pattern");
          ep = luaI_classend(ms, p);  /* points to what is next */
          METER(ms, ::eawr::script::sflua_metering::saturating_multiply(2, static_cast<::eawr::script::sflua_metering::StepUnits>(ep-p)));
          previous = (s == ms->src_init) ? '\0' : *(s-1);
          if (matchbracketclass(uchar(previous), p, ep-1) ||
             !matchbracketclass(uchar(*s), p, ep-1)) return NULL;
          p=ep; goto init;  /* else return match(ms, s, ep); */
        }
        default: {
          if (isdigit(uchar(*(p+1)))) {  /* capture results (%0-%9)? */
            s = metered_match_capture(ms, s, *(p+1));
            if (s == NULL) return NULL;
            p+=2; goto init;  /* else return match(ms, s, p+2) */
          }
          goto dflt;  /* case default */
        }
      }
    }
    case '\0': {  /* end of pattern */
      return s;  /* match succeeded */
    }
    case '$': {
      if (*(p+1) == '\0')  /* is the `$' the last char in pattern? */
        return (s == ms->src_end) ? s : NULL;  /* check end of string */
      else goto dflt;
    }
    default: dflt: {  /* it is a pattern item */
      const char *ep = luaI_classend(ms, p);  /* points to what is next */
      int m = s<ms->src_end && (METER(ms, ep-p), luaI_singlematch(uchar(*s), p, ep));
      switch (*ep) {
        case '?': {  /* optional */
          const char *res;
          if (m && ((res=metered_match(ms, s+1, ep+1)) != NULL))
            return res;
          p=ep+1; goto init;  /* else return match(ms, s, ep+1); */
        }
        case '*': {  /* 0 or more repetitions */
          return metered_max_expand(ms, s, p, ep);
        }
        case '+': {  /* 1 or more repetitions */
          return (m ? metered_max_expand(ms, s+1, p, ep) : NULL);
        }
        case '-': {  /* 0 or more repetitions (minimum) */
          return metered_min_expand(ms, s, p, ep);
        }
        default: {
          if (!m) return NULL;
          s++; p=ep; goto init;  /* else return match(ms, s+1, ep); */
        }
      }
    }
  }
}


/* lmemfind without memchr, so that each position is charged before it is
   tried */
static const char *metered_lmemfind (lua_State *L, const char *s1, size_t l1,
                                       const char *s2, size_t l2) {
  if (l2 == 0) return s1;  /* empty strings are everywhere */
  else if (l2 > l1) return NULL;  /* avoids a negative `l1' */
  else {
    const char *last = s1 + (l1 - l2);  /* `s2' cannot be found after that */
    for (; s1 <= last; s1++) {
      sandbox_charge_steps(L, 1);
      if (*s1 == *s2) {
        sandbox_charge_steps(L, static_cast<::eawr::script::sflua_metering::StepUnits>(l2 - 1));
        if (memcmp(s1+1, s2+1, l2-1) == 0)
          return s1;
      }
    }
    return NULL;  /* not found */
  }
}


static int metered_str_find (lua_State *L) {
  size_t l1, l2;
  const char *s = luaL_checklstring(L, 1, &l1);
  const char *p = luaL_checklstring(L, 2, &l2);
  sint32 init = posrelat(luaL_optlong(L, 3, 1), l1) - 1;
  if (init < 0) init = 0;
  else if ((size_t)(init) > l1) init = (sint32)l1;
  if (lua_toboolean(L, 4) ||  /* explicit request? */
      strpbrk(p, SPECIALS) == NULL) {  /* or no special characters? */
    /* do a plain search */
    const char *s2 = metered_lmemfind(L, s+init, l1-init, p, l2);
    if (s2) {
      lua_pushnumber(L, (lua_Number)(s2-s+1));
      lua_pushnumber(L, (lua_Number)(s2-s+l2));
      return 2;
    }
  }
  else {
    MatchState ms;
    int anchor = (*p == '^') ? (p++, 1) : 0;
    const char *s1=s+init;
    ms.L = L;
    ms.src_init = s;
    ms.src_end = s+l1;
    do {
      const char *res;
      ms.level = 0;
      if ((res=metered_match(&ms, s1, p)) != NULL) {
        lua_pushnumber(L, (lua_Number)(s1-s+1));  /* start */
        lua_pushnumber(L, (lua_Number)(res-s));   /* end */
        return push_captures(&ms, NULL, 0) + 2;
      }
    } while (s1++<ms.src_end && !anchor);
  }
  lua_pushnil(L);  /* not found */
  return 1;
}


static int metered_gfind_aux (lua_State *L) {
  MatchState ms;
  const char *s = lua_tostring(L, lua_upvalueindex(1));
  size_t ls = lua_strlen(L, lua_upvalueindex(1));
  const char *p = lua_tostring(L, lua_upvalueindex(2));
  const char *src;
  ms.L = L;
  ms.src_init = s;
  ms.src_end = s+ls;
  for (src = s + (size_t)lua_tonumber(L, lua_upvalueindex(3));
       src <= ms.src_end;
       src++) {
    const char *e;
    ms.level = 0;
    if ((e = metered_match(&ms, src, p)) != NULL) {
      int newstart = e-s;
      if (e == src) newstart++;  /* empty match? go at least one position */
      lua_pushnumber(L, (lua_Number)newstart);
      lua_replace(L, lua_upvalueindex(3));
      return push_captures(&ms, src, e);
    }
  }
  return 0;  /* not found */
}


static int metered_gfind (lua_State *L) {
  luaL_checkstring(L, 1);
  luaL_checkstring(L, 2);
  lua_settop(L, 2);
  lua_pushnumber(L, 0);
  lua_pushcclosure(L, metered_gfind_aux, 3);
  return 1;
}


/* the bytes push_captures copies, charged before it copies them */
static ::eawr::script::sflua_metering::StepUnits captures_length (MatchState *ms, const char *s, const char *e) {
  ::eawr::script::sflua_metering::StepUnits total = 0;
  int i;
  if (ms->level == 0 && s) return static_cast<::eawr::script::sflua_metering::StepUnits>(e-s);
  for (i=0; i<ms->level; i++)
    if (ms->capture[i].len >= 0) total = ::eawr::script::sflua_metering::saturating_add(
        total, static_cast<::eawr::script::sflua_metering::StepUnits>(ms->capture[i].len));
  return total;
}


static void metered_add_s (MatchState *ms, luaL_Buffer *b,
                             const char *s, const char *e) {
  lua_State *L = ms->L;
  if (lua_isstring(L, 3)) {
    const char *news = lua_tostring(L, 3);
    size_t l = lua_strlen(L, 3);
    size_t i;
    for (i=0; i<l; i++) {
      if (news[i] != ESC)
        luaL_putchar(b, news[i]);
      else {
        i++;  /* skip ESC */
        if (!isdigit(uchar(news[i])))
          luaL_putchar(b, news[i]);
        else {
          int level = check_capture(ms, news[i]);
          if (ms->capture[level].len > 0) METER(ms, ms->capture[level].len);
          push_onecapture(ms, level);
          luaL_addvalue(b);  /* add capture to accumulated result */
        }
      }
    }
  }
  else {  /* is a function */
    int n;
    lua_pushvalue(L, 3);
    METER(ms, captures_length(ms, s, e));
    n = push_captures(ms, s, e);
    lua_call(L, n, 1);
    if (lua_isstring(L, -1)) {
      METER(ms, lua_strlen(L, -1));
      luaL_addvalue(b);  /* add return to accumulated result */
    }
    else
      lua_pop(L, 1);  /* function result is not a string: pop it */
  }
}


static int metered_str_gsub (lua_State *L) {
  size_t srcl;
  const char *src = luaL_checklstring(L, 1, &srcl);
  const char *p = luaL_checkstring(L, 2);
  int max_s = luaL_optint(L, 4, srcl+1);
  int anchor = (*p == '^') ? (p++, 1) : 0;
  int n = 0;
  MatchState ms;
  luaL_Buffer b;
  luaL_argcheck(L,
    lua_gettop(L) >= 3 && (lua_isstring(L, 3) || lua_isfunction(L, 3)),
    3, "string or function expected");
  luaL_buffinit(L, &b);
  ms.L = L;
  ms.src_init = src;
  ms.src_end = src+srcl;
  while (n < max_s) {
    const char *e;
    ms.level = 0;
    e = metered_match(&ms, src, p);
    if (e) {
      n++;
      if (lua_isstring(L, 3)) METER(&ms, lua_strlen(L, 3));
      metered_add_s(&ms, &b, src, e);
    }
    if (e && e>src) /* non empty match? */
      src = e;  /* skip it */
    else if (src < ms.src_end)
      luaL_putchar(&b, *src++);
    else break;
    if (anchor) break;
  }
  luaL_addlstring(&b, src, ms.src_end-src);
  luaL_pushresult(&b);
  lua_pushnumber(L, (lua_Number)n);  /* number of substitutions */
  return 2;
}

#undef METER

lua_CFunction sflua_metered_find() { return metered_str_find; }
lua_CFunction sflua_metered_gfind() { return metered_gfind; }
lua_CFunction sflua_metered_gfind_iterator() { return metered_gfind_aux; }
lua_CFunction sflua_metered_gsub() { return metered_str_gsub; }
EAWR_SFLUA_END
