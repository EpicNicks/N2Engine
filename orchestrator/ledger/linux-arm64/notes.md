# Notes (linux-arm64, 2026-10-11)
Approved deviations (accepted by the user):
- aarch64 runs scalar only (no NEON); SSE2/SSE4.1 entry points are inline aliases of the scalar versions on non-x86
- Vector3/Vector4/Quaternion union storage is a 16-byte-aligned float[4] struct (SimdStorage) on non-x86
- No engine/renderer/editor-server changes were needed
- GCC informational leg fails as before (known sol2 bug)
Open low (folded into the linux-docs-deploy docs PR): docs/testing.html line ~290 says the TLS smoke runs only on the Windows TLS ON leg and the Linux Clang leg; the arm64 Clang leg runs it too.
No NEON path: aarch64 is scalar only. Unverified: external/ dependencies not inspected for x86-only flags (arm64 CI built them fine)
