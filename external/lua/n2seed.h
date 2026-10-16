/*
** N2Engine addition (not part of Lua 5.4.8): the one switch behind the two places where Lua seeds itself from the
** clock and from addresses, the string hash seed (luai_makeseed in lstate.c) and the math.random state (randseed in
** lmathlib.c). Unset (the default) leaves both exactly as upstream Lua has them. Set, both are a function of the
** seed alone. Recorded in THIRD_PARTY_NOTICES.md with the two patches that call it.
**
** Not thread safe: set it from the main thread before the first lua_State it should affect is created. States that
** already exist keep the seeds they were created with.
*/
#ifndef n2seed_h
#define n2seed_h

#ifdef __cplusplus
extern "C" {
#endif

/* enabled != 0: states created from now on use `seed`; enabled == 0: upstream behaviour (clock and addresses) */
void n2lua_setseed (int enabled, unsigned long long seed);

/* 1 and *seed set if a seed is configured, else 0 and *seed untouched */
int n2lua_getseed (unsigned long long *seed);

#ifdef __cplusplus
}
#endif

#endif
