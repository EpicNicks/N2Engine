/*
** N2Engine addition: see n2seed.h.
*/

#include "n2seed.h"

static int n2_enabled = 0;
static unsigned long long n2_seed = 0;

void n2lua_setseed (int enabled, unsigned long long seed) {
  n2_enabled = (enabled != 0);
  n2_seed = n2_enabled ? seed : 0;
}

int n2lua_getseed (unsigned long long *seed) {
  if (!n2_enabled)
    return 0;
  *seed = n2_seed;
  return 1;
}
