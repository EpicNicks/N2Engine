Accepted by the user 2026-10-13:
- strict FP set on the existing legs (windows Release via matrix key, linux x64/arm64 Clang, web dev profile) instead of new dedicated legs; includes the required linux Clang check
- corpus is a gtest in engine_tests (also run by ctest), file written only when N2_DETERMINISM_HASHES_OUT is set
- plan item 11 (thread counts 0 vs 4) not a corpus scene; web speed profile not run (builds without tests)
- PhysX control runs scenes 1-3 only, hashed from GetBodyState
- no golden committed (cannot be produced locally): job compares legs only and uploads a golden-candidate artifact; commit it from the first green run
- determinism job also runs when a leg failed; missing leg/scene/record is a finding
