/* The one translation unit that compiles ufbx (compiled as C, as ufbx is written). Only built with
 * N2ENGINE_MODEL_UFBX (assetimport/CMakeLists.txt leaves it out otherwise). Nothing outside assetimport/src includes
 * ufbx: the library's public API exposes no ufbx type. ufbx_load_file (which uses fopen) is compiled but never
 * called: UfbxImporter.cpp reads every file itself, with size caps and a "stays in the model's folder" check, and
 * loads with load_external_files off. */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#ifdef _MSC_VER
#pragma warning(push, 0)
#endif

#include <ufbx/ufbx.c>

#ifdef _MSC_VER
#pragma warning(pop)
#endif
