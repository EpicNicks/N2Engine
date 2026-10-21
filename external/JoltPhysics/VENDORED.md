# Jolt Physics, vendored

| | |
|---|---|
| Upstream | https://github.com/jrouwe/JoltPhysics |
| Version | v5.6.0 (tag `v5.6.0`, released 2026-07-11) |
| Commit | `e77f175595e64cb44218cc9d9d56fc365ad0e36a` (`git clone --depth 1 --branch v5.6.0`, `git rev-parse HEAD`) |
| Upstream `Jolt/` tree | `41598db7d35e7716abf3021e9c73c42c2526b4be` (`git rev-parse v5.6.0:Jolt`) |
| Upstream `LICENSE` blob | `4f0976848529e8243687918193e0742032aef7dc` |
| Vendored on | 2026-10-20 |
| Licence | MIT, `Copyright 2021 Jorrit Rouwe` (the file `LICENSE`, unchanged) |

## What is here

- `Jolt/`: the whole upstream `Jolt/` directory, 559 files, byte for byte (this includes `Jolt/Jolt.cmake`, which
  `cmake/jolt/CMakeLists.txt` includes; the compute shaders and compute sources are in it too and are not compiled).
- `LICENSE`: the upstream licence file, byte for byte.
- `SHA256SUMS`: the SHA-256 of each of those 560 files (`sha256sum -c SHA256SUMS` from this folder). `scripts/check-jolt-vendor.sh`
  runs that check and also fails on a file under `Jolt/` that the list does not have. The hashes were also compared with
  upstream's git blob hashes (`git ls-tree -r v5.6.0 Jolt LICENSE` against `git hash-object --no-filters` of each file here):
  all 560 equal.

## What was pruned (not vendored)

Everything else in the upstream repository: `Assets/`, `Build/`, `Docs/`, `HelloWorld/`, `JoltViewer/`,
`PerformanceTest/`, `Samples/`, `TestFramework/`, `UnitTests/`, `.github/`, `ContributorAgreement.md`, `Doxyfile`,
`README.md`, `doxygen-awesome.css`, `run_doxygen.bat`, `sonar-project.properties`, `.clang-format`, `.editorconfig`,
`.gitattributes`, `.gitignore`. The build options that upstream's `Build/CMakeLists.txt` sets are set in this
repository's `cmake/jolt/CMakeLists.txt` instead (which says which are Jolt's and which are ours).

## Local patches

None. The files kept are exactly upstream's. Added here, not from upstream: `.gitignore` (the repository ignores
`*.cmake`; this file un-ignores Jolt's own `Jolt/Jolt.cmake`), `SHA256SUMS` and this file. The repository's
`.gitattributes` has `external/** -text`, so git stores and checks out these files without any line-ending conversion
(the upstream files are LF).

## Updating

Clone the new tag outside the repository, replace `Jolt/` and `LICENSE`, regenerate `SHA256SUMS`
(`(find Jolt -type f; echo LICENSE) | LC_ALL=C sort | xargs sha256sum > SHA256SUMS`), update this file and the entry in
`THIRD_PARTY_NOTICES.md`. A new version can change the simulation: `kGoldenStateHash` in `tests/jolt/JoltScenario.hpp`
changes only in a change that says why.
