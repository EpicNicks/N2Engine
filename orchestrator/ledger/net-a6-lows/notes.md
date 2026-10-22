Accepted by the user 2026-10-13:
- Item 2: corrected the docs instead of suppressing the Debug console helper, because the helper is also what redirects std::cout (the SIMD report) into the logger in Debug, and the SIMD report reaches stdout in every build
- Item 4: made the drop real (queued, Disconnect after Poll) but added no test: the branch is unreachable (identity is always set from the matched token entry) and RunServer has no injection point
- Item 1: no test, RunServer has no injectable failure; early scene-load and listen failure paths now use the same guarded shutdown lambda
- Review had only 2 lows (verdict clean); fixed both anyway: stopEngine before the refused-project return 1, docs sentence on Debug echo timing
