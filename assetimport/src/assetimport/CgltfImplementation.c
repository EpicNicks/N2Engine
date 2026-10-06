/* The one translation unit that compiles cgltf (compiled as C, as cgltf is written). Nothing outside
 * assetimport/src includes cgltf: the library's public API exposes no cgltf type. cgltf_parse_file and
 * cgltf_load_buffers (which use fopen) are compiled but never called: GltfImporter.cpp reads every file itself,
 * with size caps and a "stays in the model's folder" check. */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#ifdef _MSC_VER
#pragma warning(push, 0)
#endif

#define CGLTF_IMPLEMENTATION
#include <cgltf/cgltf.h>

#ifdef _MSC_VER
#pragma warning(pop)
#endif
