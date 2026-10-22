Netcode A5 (netcode design 2.1.3 and the A5 row of 7.2): the connection handshake with a stateless challenge cookie, per-connection keys, AEAD-sealed packets on Mbed TLS primitives, join tokens, and the editor host's security policy classes moved into a shared library. **Needs the user's security-review accept before merge** (section "Security review" below).

## What changed

**Build**
- `cmake/mbedtls/CMakeLists.txt`: Mbed TLS 3.6.7 (vendored, unmodified) is built once for the tree. `n2_mbedcrypto` (crypto modules only, no X.509/TLS) in every native build; `n2_mbedtls_tls` (X.509 + TLS) only with `N2ENGINE_EDITOR_TLS`. One configuration for both: the default plus `cmake/mbedtls/config/n2_mbedtls_user_config.h` (moved from `editor-server/mbedtls-config/`; threading on). The web build adds neither and still compiles (`N2SECURITY_HAS_CRYPTO=0`, `N2NET_HAS_SECURE=0`).
- `N2EditorServer` no longer compiles the Mbed TLS sources itself; it links `n2_mbedtls_tls` with the option. The `N2ENGINE_EDITOR_TLS` option moved to the root `CMakeLists.txt`.

**n2security** (new target, `engine/include/engine/security/`, `engine/src/security/`)
- `PeerAddress`/`RateLimiter`, `TokenBucket`, `PreAuthSlots`: moved from `editor-server/` with only the namespace changed (`N2Engine::Security`, plus a header comment); `editor-server/`'s headers of the same names re-export them into `N2Engine::Editor`.
- `TokenTable.hpp`: the editor's parsing, file checks and constant-time matching, generic over role names (`BasicTokenTable<Traits>`). The editor's `TokenTable` is that with viewer/editor/admin and its own error hint (the error texts are byte-identical); its `HashToken`/`GenerateToken` stay in the editor host (they still need `N2ENGINE_EDITOR_TLS`).
- `Crypto.hpp`: wrappers (SHA-256, HMAC-SHA-256, HKDF-SHA-256, CTR_DRBG, X25519, ChaCha20-Poly1305, constant-time compare, zeroize) and `InitializeCrypto` (the threading callbacks + `psa_crypto_init`, once per process; `EditorTls.cpp` now calls it instead of its own copy).
- Editor host: no behaviour change. Its test sources are untouched; `tests/editor-server/CMakeLists.txt` links `n2_mbedtls_tls` instead of listing the Mbed TLS include paths.

**n2net**
- `SecureTransport` (an `ITransport` over another one): the handshake, sealed packets, replay window, admission. `OpenUdp` is the only caller that opens a `UdpTransport` with `encryptionAvailable = true` (A4's fail-closed rule: non-loopback only with the secure layer, otherwise `insecureBind` is still required).
- `JoinTokens.hpp`: `JoinRole` (player, spectator, admin), `JoinTokenTable`, `HashJoinToken`.
- `ITransport::GetPeerEndpoint` (default false; `UdpTransport` and `SimTransport` implement it; `SimNetwork::SetEndpointAddress`).
- `ConnectionConfig::maxPacketSize` (default 1200, clamped 128-1200) so `Connection` builds packets that fit after the 25-byte seal; `Connection::MaxUnreliableMessageSize()`.

**Tests**: `tests/engine/network/n2net/N2NetSecureTransportTests.cpp` (in `engine_tests`): known-answer vectors of every primitive, join tokens, the replay window, the handshake (success, wrong token, no slot or session before the echo with slot counts at every step, pending timeout, forged / other-address / altered / expired cookies, secret rotation, the amplification bound with junk of every type, a lost Accept), sealed packets (replay, too old, tampered ciphertext/tag/type/sequence/length, sequence uniqueness), a connect flood, and end to end over loopback UDP with the A3 channels (a fragmented message included) plus the non-loopback bind rule. SimNetwork with a virtual clock, no sleeps; the UDP test has a 20 s deadline.

**Docs**: `docs/netcode.html` (new section "The secure layer": handshake diagram, sealed packets, admission, join tokens, crypto choices, trust table, threat-model coverage, n2security, limits; updated overview, transport interface, fail-closed rule, UDP limits, tests, not-yet), `docs/testing.html`, `docs/logging-and-editor.html` and `docs/editor.html` (moved source paths, the Mbed TLS build).

## Security review

**Primitives** (all from the vendored Mbed TLS 3.6.7 default config, verified in `mbedtls_config.h`: `MBEDTLS_ECP_DP_CURVE25519_ENABLED`, `MBEDTLS_ECDH_C`, `MBEDTLS_CHACHAPOLY_C`, `MBEDTLS_HKDF_C`, `MBEDTLS_SHA256_C`, `MBEDTLS_CTR_DRBG_C`; `Crypto.cpp` stops the build with `#error` if any is missing):

| Use | Primitive | Parameters |
|---|---|---|
| Key exchange | X25519 (RFC 7748) via `mbedtls_ecdh_gen_public` / `mbedtls_ecdh_compute_shared` on `MBEDTLS_ECP_DP_CURVE25519` | ephemeral per handshake on both sides; peer key checked with `mbedtls_ecp_check_pubkey`; all-zero shared secret refused; private keys wiped after use |
| Session keys | HKDF-SHA-256 (RFC 5869) | salt = transcript hash T; IKM = the X25519 secret; info = `"n2net v1 client to server key"` / `"n2net v1 server to client key"`; 32 bytes each |
| Packets | ChaCha20-Poly1305 (RFC 8439) | 32-byte key per direction; nonce = direction byte (0x43 C->S, 0x53 S->C) + 3 zero bytes + u64 LE sequence; AD = type, version, u32 protocol id, u64 sequence; 16-byte tag |
| Cookie | HMAC-SHA-256 | key = 32-byte per-process random secret, rotated every 60 s (the previous epoch's secret kept); MAC over label, address family, 16 address bytes, port, u64 server time (ms), version, protocol id, client public key, proof1; lifetime 5 s |
| Token proofs | HMAC-SHA-256 keyed with K = SHA-256(token) | proof1 over label + version + protocol id + client public key; proof2 over label + cookie time + cookie; server proof over label + T |
| Transcript T | SHA-256 | label + response bytes 1..141 (version through proof2) + server public key |
| RNG | CTR_DRBG (AES-256) seeded from `mbedtls_entropy_func` (BCrypt / the kernel) | one per process, under a mutex |
| Secret compares | own OR-accumulate loop over every byte (no early exit); the Poly1305 tag check inside the library | |

**Handshake**: ConnectRequest (256 bytes, zero padded) -> Challenge (41) -> ConnectResponse (256) -> Accept (82; its payload sealed with the S->C key at sequence 0) -> the client's first sealed packet (a keep-alive). Domain-separation labels on every MAC/hash/KDF input.

**Authenticated / not authenticated**
- Authenticated: every sealed packet (header in AD, payload under the tag); the Accept's role and server proof (sealed, and the proof is over T); the client's knowledge of K (proof1 and proof2, matched against every token entry in constant time with mask selection); the cookie's binding to source address and port, time and request fields.
- Not authenticated: the Challenge (a client cannot verify it; the client takes only the first Challenge per request round, so a forged one arriving before the real one makes the handshake fail, never succeed wrongly); the ConnectRequest (anyone may send one; it costs a rate token and one HMAC, no state); packet sizes, timing, types and sequence numbers (visible).
- Server identity: only "holds K for my token". No certificates. Whoever has the tokens file (or the token) can impersonate the server to that client, and that user to the server.

**Properties claimed (and tested)**
- No server state before a valid cookie echo (session and pre-auth slot counts asserted at every step).
- Amplification: both client handshake packets must be exactly 256 bytes; the only pre-session replies are 41 and 82 bytes; static_asserts plus a runtime check in `SendPreAuthReply`; tested with junk of every type and size.
- Nonce uniqueness: fresh keys per session and per direction; the sequence is consumed before sealing and never wraps (sealing refused at 2^64-1); the only datagram sent twice (a lost Accept, at most 8 times) is byte-identical.
- Replay: 256-packet window, checked before decrypting, updated only after the tag verified.
- Silent drops for everything that fails (no denied packet: it could not be authenticated, and a forgeable one would let anyone abort handshakes); counters only; this layer has no logging at all.
- Tokens: the client never sends the token or K; the transport wipes the token text from its copy of the options; tokens files keep the editor's checks (1 MiB cap, POSIX owner-only permissions, errors never repeat a hash).
- Forward secrecy: ephemeral X25519 per session; the client wipes its private key once the Accept verifies, the server right after deriving the keys.
- Admission (the reused editor policy, keyed on source IP): RateLimiter backoff for wrong tokens and unanswered Accepts (2 free failures, 1 s doubling to 5 min); TokenBucket 200/s + 10/s per address for Challenges, 50/s + 5/s per address for echoes, charged only after the echo's cookie verifies (spoofed echoes cannot spend it); PreAuthSlots 16 in all, 2 per address, from the echo to the first sealed packet; at most 64 sessions. Loopback is exempt from the per-address rules, as in the editor host.
- Every Mbed TLS return code checked; contexts freed by RAII on every path; secrets wiped with `mbedtls_platform_zeroize`.

**Known limits**
- Offline guessing: the proofs are MACs of public data keyed with K, so a passive observer can test guesses of a low-entropy token. Tokens must be long random strings (no generator in this PR; A6 adds one, like the editor's 256-bit `--new-token`).
- Whoever has the tokens file can impersonate any user and the server (the same as the editor host's file: it is the credential store).
- A spoofed-source flood can still fill the inner `UdpTransport` peer table (A4's cap and idle timeout bound it).
- An on-path attacker, or one who can spoof the server's address, can make handshakes fail (forged Challenges, drops); it cannot make one succeed.
- No re-keying, no address migration (a session is tied to address and port), no revocation of established sessions when the file changes, no certificate identity for the server.
- The handshake does not agree the `Connection` id or settings yet (both sides configure them).

**Departures from netcode.io** (reviewed against it as the design asked): keys from an ephemeral X25519 exchange instead of a backend-issued encrypted connect token (v1 has no backend, only a static tokens file); an HMAC cookie instead of an encrypted challenge token (the echo carries the request fields; nothing in it is secret); no connection-denied packet; fixed 8-byte sequences instead of 1-8; 256-byte padded requests instead of 1078 (still larger than every reply). ChaCha20-Poly1305 chosen over AES-GCM (both are enabled) for constant-time software on every leg.

## Review round 1 fixes
- high: the client ignores further Challenges once it has echoed a cookie (a round trip over the 250 ms resend interval used to replace the cookie and make every Accept fail); test `AHandshakeCompletesWhenTheRoundTripExceedsTheResendInterval` (150 ms each way).
- medium: the connect-rate TokenBucket is taken after the cookie verifies, so spoofed echoes (one HMAC each) cannot exhaust the global budget; test `SpoofedEchoesDoNotSpendTheConnectRate` (80 forged echoes from 80 addresses, then a legitimate handshake).

## Deviations / judgement calls
- Two proofs instead of one: the request carries proof1 (as the brief asks), and the echo adds proof2 over the cookie, so a captured handshake cannot be replayed with a fresh cookie by a token-less attacker to hold pre-auth slots.
- The pre-auth slot is held until the client's first sealed packet (key confirmation), not only through the key exchange, so an abandoned handshake costs a slot only for the 5 s handshake timeout.
- `TokenTable` became a template over role traits (`BasicTokenTable`) so the editor keeps its enum and messages; the editor's hashing and token generation stay editor-side, so the editor's no-TLS behaviour (no SHA-256, `--new-token` refused) is unchanged although every native build now has the crypto library.
- The threading callbacks moved from `EditorTls.cpp` into `Security::InitializeCrypto` (one Mbed TLS build and one configuration per process, so one setup).
- `ConnectionConfig::maxPacketSize` added (the A3 layer always built 1200-byte packets; the seal needs 25 bytes of room).
- `ITransport::GetPeerEndpoint` added with a default (false), rather than parsing `UdpTransport::GetPeerInfo` text.
- `ConstantTimeEqual` is our own loop: `mbedtls/constant_time.h` has no C++ linkage guard.
- No join-token generator in this PR (A6, the dedicated server, is where a CLI belongs).

## Not verified
- Behaviour across real machines, NATs or lossy links (only loopback and the simulator).
- Constant-time behaviour is by construction and review, not measured.
- No independent security review yet (this PR asks for it).

## Approved deviations
Accepted by the user 2026-10-13:
- Two proofs: request carries proof1, cookie echo adds proof2 over the cookie so a captured handshake cannot be replayed with a fresh cookie to hold pre-auth slots
- Pre-auth slot held until the client first sealed packet (key confirmation); abandoned handshake costs a slot only for the 5 s handshake timeout
- TokenTable became a template over role traits (BasicTokenTable); editor hashing/token generation stay editor-side so editor no-TLS behaviour is unchanged
- Mbed TLS threading callbacks moved from EditorTls.cpp into Security::InitializeCrypto (one crypto build/config per process)
- ConnectionConfig::maxPacketSize added (seal needs 25 bytes of room under the 1200-byte A3 packet)
- ITransport::GetPeerEndpoint added with a default (false) instead of parsing UdpTransport::GetPeerInfo text
- ConstantTimeEqual is our own loop: mbedtls/constant_time.h has no C++ linkage guard
- No join-token generator CLI in this PR (deferred to A6 dedicated server)
- Departures from netcode.io: ephemeral X25519 keys instead of backend-issued connect token; HMAC cookie instead of encrypted challenge token; no connection-denied packet; fixed 8-byte sequences; 256-byte padded requests
- ChaCha20-Poly1305 chosen over AES-GCM (both enabled in the vendored config)
- Client takes only the first Challenge per request round (ignores later ones while Responding); a Challenge after a cookie-lifetime re-request still replaces the cookie
- Connect-rate TokenBucket now charged after cookie verification, so ConnectResponse cookie checks (one HMAC) are bounded only by backoff, not by a bucket

