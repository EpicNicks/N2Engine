# Notes (gcc-sol2-lambda-collision, 2026-10-13)
Approved deviations (accepted by the user):
- Chose option 1: GCC-only (__GNUC__ && !__clang__) typeid suffix when the name contains 'lambda('; sol2 3.2.3 has no upstream fix/macro
- Added #include <typeinfo> to the vendored sol.hpp
- GCC leg now runs the two Python smoke steps; leg renamed 'linux (Release, x86_64, GCC 14)'
- editor.html says GCC hosts work but Clang remains the documented/Dockerfile path
- deploy/Dockerfile is outside the allowed paths; edited its comment only (reviewer low finding) - revert if unwanted
- Regression test now pushes lambdas as values (create_table_with, table.set, capturing lambdas); not verified against a revert of the patch, relies on GCC leg
- docs/testing.html callout should still be checked: not re-edited this round
Re-apply the vendored sol2 patch if sol2 is updated and still has the collision. GCC leg `linux (Release, x86_64, GCC 14)` passes all Lua tests and is not a required check. Unverified: Nothing GPU related; New test fails without the sol.hpp patch on GCC: not run, CI only
