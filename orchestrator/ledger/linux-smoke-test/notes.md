# Notes (linux-smoke-test, 2026-10-10)
Approved deviations (accepted by the user):
- Both pwsh smoke steps deleted after the Python steps ran green beside them on all Windows legs and the Clang leg; every assertion mapped in the PR body
- Added assertions: user:// parent equals the user data folder, key mode 0600 on POSIX, key file holds a private key, TLS identity reused by a second host
- Hosts get stdin from NUL/devnull (pwsh inherited the runner stdin); logs go to a temp work folder, not the repo root
- TLS client is Python ssl (OpenSSL) instead of .NET SslStream
- Added ALSOFT_DRIVERS=null to the Linux smoke steps
- docs/testing.html also gained a short Linux CI paragraph, which it lacked after #113
- PR left non-draft; GCC leg still red (known sol2 bug, informational)
Unverified: Windows ACLs on the TLS identity folder and key; XDG_DATA_HOME branch of the user data folder in the smoke test (runner has HOME only)
