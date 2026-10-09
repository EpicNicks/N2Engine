# ufbx

`include/ufbx/ufbx.h` and `include/ufbx/ufbx.c` are ufbx v0.23.1, copied unmodified from
https://github.com/ufbx/ufbx at the `v0.23.1` tag (the single-file release: `ufbx.c` and `ufbx.h`). Dual licensed:
MIT or public domain (Unlicense), at your choice: `LICENSE` here, and the notice at the end of `ufbx.c`.

It is optional. It is compiled only when the CMake option `N2ENGINE_MODEL_UFBX` is ON (off by default; the CI leg that
builds FreeType turns it on), once, as C, in `assetimport/src/assetimport/UfbxImplementation.c`. Only
`assetimport/src/assetimport/UfbxImporter.cpp` includes `ufbx.h`: the `assetimport` library's public API exposes no ufbx
type. ufbx's own file loading (`ufbx_load_file`) is never called; the importer reads every file itself, under size caps
and the rule that a path must stay inside the model's folder, and ufbx loads with `load_external_files` off.
