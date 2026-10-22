Corrects the three accuracy lows left in docs/web-build.html after PR #184.

- PhysX tests under node: the hard-coded counts (1504 / 1490 / 14, web_tests 18) are removed; the text names which tests are skipped and why, and points to the web job log for counts.
- Cold-cache cost: the design-decision callout said about four minutes; it now says about two minutes (102 s PhysX build, 122 s first configure), matching the CI bullet.
- Stage 6: states that not defining N2ENGINE_SINGLE_THREADED does not bring PhysX workers back; the web PhysX libraries are built without -pthread (scripts/web-build-physx.sh) and neither the .pin stamp nor the cache key (tests.yml) records threading, so a threaded artifact needs its own keyed PhysX build.

docs/testing.html and docs/physics.html checked: no stale counts or claims found.

Not verified
- Current counts from a fresh web CI log (counts were removed instead).
