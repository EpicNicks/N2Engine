# Box2D, vendored

| | |
|---|---|
| Upstream | https://github.com/erincatto/box2d |
| Version | v3.2.0 (tag `v3.2.0`, released 2026-10-19) |
| Commit | `178495ba35695aefe9c321e569d2277f06db3b90` (`git clone --depth 1 --branch v3.2.0`, `git rev-parse HEAD`) |
| Upstream `src/` tree | `04bb0e4d9954a51d60763b109af91a26f24eb6d2` (`git rev-parse v3.2.0:src`) |
| Upstream `include/` tree | `433648a579505073448c1c52bf1879f2c656a950` (`git rev-parse v3.2.0:include`) |
| Upstream `LICENSE` blob | `e32198ea937cdcfb5028c149b97ea211d8064da6` |
| Vendored on | 2026-11-08 |
| Licence | MIT, `Copyright (c) 2022 Erin Catto` (the file `LICENSE`, unchanged) |

## What is here

- `src/`, `include/`: the whole upstream directories, byte for byte. `src/CMakeLists.txt`, `src/box2d.natvis` and
  `src/box2d.pc.in` are kept so the directory stays identical to upstream; they are not used.
- `shared/determinism.c`, `shared/determinism.h`: upstream's determinism scene and golden, byte for byte. Only
  `tests/box2d` compiles them; they are not in the library.
- `LICENSE`: the upstream licence file, byte for byte.
- `SHA256SUMS`: the SHA-256 of each of those 87 files. `scripts/check-box2d-vendor.sh` runs that check and also fails on a
  file the list does not have. Each file was also compared with its upstream git blob when vendored
  (`git ls-tree -r v3.2.0` against `git hash-object --no-filters`): all 87 equal.

The files are the git blobs (LF line endings), written with `git cat-file blob`. A plain copy from a Windows checkout of
upstream is not identical: upstream's `.gitattributes` has `* text=auto`, which gives CRLF there. The repository's
`.gitattributes` keeps `external/**` as `-text`, so the bytes stay as they are.

## Local patches

None.

## What was pruned (not vendored)

Everything else in the upstream repository: `api/`, `benchmark/`, `docs/`, `extern/`, `samples/`, `test/`, the rest of
`shared/`, `.github/`, the top-level `CMakeLists.txt`, `CMakePresets.json`, `Package.swift`, `build.zig`, `build.zig.zon`,
`README.md` and the build scripts. The build options are set in `cmake/box2d/CMakeLists.txt` instead.

## Upgrading

Replace the files from the new tag's blobs, regenerate `SHA256SUMS`, update this file, and run `tests/box2d` on every CI
leg. Box2D's golden changes between versions, and `kGoldenStateHash50Hz` in `tests/box2d/Box2DSmokeTests.cpp` is expected to
change with it. Measure it on several legs before writing it down.
