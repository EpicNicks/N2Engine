# Notes (linux-ci-leg, 2026-10-10)
Required check name: `linux (Release, x86_64, Clang)` (Clang 20 from apt.llvm.org, libstdc++ 14). The GCC 14 leg is informational (continue-on-error) and shows a red check on every PR until the sol2 same-signature-lambda collision under GCC is fixed (3 Lua tests, e.g. Layers.MaskOf ran LayerToName); GCC-built hosts are unsupported until then.
Approved deviations (accepted by the user):
- Required job is Clang 20 (apt.llvm.org) + libstdc++ 14 via --gcc-install-dir, not Clang 18 (lacks std::expected with libstdc++ 14)
- GCC 14 kept as separate informational leg (continue-on-error), known sol2 lambda-collision bug documented in PR body and workflow comments
- Edited vendored external/sol2/include/sol/sol.hpp (optional<T&>::emplace body) - outside the listed allowed paths; Clang hard-errors on it, never instantiated
- Over-the-socket hardening tests now run on Linux too (were #ifdef _WIN32)
- GLFW Wayland OFF on Linux (X11 only)
- VK_USE_PLATFORM_XLIB_KHR dropped on Linux (breaks CullMode::None via Xlib macros)
- ComponentRegistry.hpp now includes engine/Component.hpp
- Merged origin/master (PR #114); its DroppedClient and UserDataBase tests pass on the Clang leg unchanged
- No docs/ page edited (outside allowed paths); the note lives in the PR body and workflow comments
- Review lows: marker comment at the sol2 emplace patch; DroppedClientTests moved onto TestSockets.hpp (added AbortOnClose helper)
Open lows: ETIMEDOUT counts as a receive timeout in TestSockets.hpp (POSIX, unreachable on loopback).
Unverified: a real Linux host run; the dropped-client test failing with the SIGPIPE fix reverted.
