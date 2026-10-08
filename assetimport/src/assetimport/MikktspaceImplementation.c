/* The one translation unit that compiles MikkTSpace (as C, as it is written). Nothing outside assetimport/src
 * includes it: the library's public API exposes no MikkTSpace type. The file is vendored unmodified. */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#ifdef _MSC_VER
#pragma warning(push, 0)
#endif

#include <mikktspace/mikktspace.c>

#ifdef _MSC_VER
#pragma warning(pop)
#endif
