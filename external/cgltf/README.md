# cgltf

`include/cgltf/cgltf.h` is cgltf v1.15, copied unmodified from https://github.com/jkuhlmann/cgltf at the `v1.15`
tag (commit `360db1a95480fe102ae9c69b27c5d101167ff5ba`). MIT licence: `LICENSE` here, and the notice at the end of
the header. cgltf bundles jsmn (MIT) for its JSON parsing.

cgltf is compiled once, as C, in `assetimport/src/assetimport/CgltfImplementation.c`. Only
`assetimport/src/assetimport/GltfImporter.cpp` includes it: the `assetimport` library's public API exposes no
cgltf type. cgltf's own file loading (`cgltf_parse_file`, `cgltf_load_buffers`) is never called; the importer
reads external buffers and images itself, under the size caps and the rule that a URI must stay inside the
model's folder.
