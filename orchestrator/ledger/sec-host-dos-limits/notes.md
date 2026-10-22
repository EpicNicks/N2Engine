Accepted by the user 2026-10-13:
- TLS handshake keeps its own 5 s budget; the 2 s Hello budget starts after the handshake (found by CI: Windows Debug smoke test, 2 s too tight)
- Prior-offender cap also applies to the handshake budget (min(5 s, 2 s)), as before the split; SetTlsHandshakeTimeout setter added (not a flag)
