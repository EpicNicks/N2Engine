# Design proposal: the gameplay netcode runtime (two built-in network presets)

Status: design only, no engine changes, nothing built or run. N2Engine `master` at `637797c` (Merge #183, 2026-10-20), clean checkout.
Inputs: `proposals/physx-limitations-and-determinism.md` (the #157 report: preset bundles 4.2, blocker inventory 5, staged plan S1-S3, D1-D7), `proposals/physx-determinism-measured-numbers.md` (PR #161 CI numbers), the user's 2026-10-20 decisions (memory `pending-minor-defects.md`, last entries).
Marks: **[V]** verified in this repo (path:line, relative to `N2Engine/`), **[M]** from memory or general knowledge, unverified here (verify before relying on it), **[E]** my estimate.

User decisions this design implements (2026-10-20): D1 three presets (None / Server authoritative / Deterministic lockstep-rollback); D2 **cross-platform determinism** (Windows + Linux + web peers); D3 Deterministic preset shown disabled with the reason while no deterministic backend exists; D4 refuse to start when a Deterministic requirement is unmet; D6 #156 evaluates Jolt / Box2D v3 before any custom engine and compares strict-float vs fixed-point; D7 an ordered-iteration helper for Lua. Clarified: BOTH presets ship built in, and the **netcode runtime itself is in scope** (replication, prediction, reconciliation, lockstep/rollback), not only the settings bundles.

---

## Executive summary

- **Nothing to extend, everything to build.** The engine has no gameplay networking [V: no `replicat|netcode|NetworkIdentity|rollback|lockstep` hits, as the #157 report found; still true at `637797c`]. The only networking is the editor host: a TCP request/response protocol (`[type u8][len u32][payload]`, one response per request, in order, `editor-server/protocol/protocol.json:4`) served one session at a time [V `docs/logging-and-editor.html:452`]. That transport is the wrong shape for a game (head-of-line blocking, request/response, one session), but its **security policy classes are reusable** (token table with roles, failed-auth backoff, token buckets, pre-auth slots: pure logic with injected clocks [V `editor-server/include/editor-server/RateLimiter.hpp:15-40`, `TokenBucket.hpp:15-31`, `TokenTable.hpp:14-60`]), and so is its fail-closed stance (`--insecure-bind`).
- **Proposed shape:** a small engine-independent library (`net/`, "n2net": transport abstraction, packet layer with reliable + unreliable channels, crypto, clock sync, snapshot/input buffers, lockstep scheduler) tested in-process on a seeded **network simulator**, plus an engine layer (`engine/src/network/`: `NetworkObject` component, field replication through the existing reflection, RPCs from C++ and Lua, the two runtimes) and a headless **`N2Server`** executable.
- **Server-authoritative preset (PhysX): buildable now.** The tick loop needs to become netcode-owned (today it is a wall-clock accumulator that drops time and scales the physics step by timeScale [V `engine/src/Application.cpp:380-410`, `engine/src/Time.cpp:60`]); prediction v1 covers owned **kinematic character movement**, not predicted rigid bodies; remote entities are snapshot-interpolated; lag compensation keeps a pose history of simple hitboxes. Two engine gaps: `IPhysicsBackend` cannot teleport a dynamic body or set its sleep state (only velocities; dynamic bodies ignore transform writes) [V `IPhysicsBackend.hpp:92-95,161-169`, `PhysXBackend.cpp:827-834`, `Rigidbody.cpp:336-338`] although PhysX has the calls [V `PxRigidActor.h:129`, `PxRigidDynamic.h:232,275,352,389`]; and there is no character controller [V: no `PxController` use in `engine/`].
- **Deterministic preset: the non-physics half can start now, the physics half waits for #156.** Lockstep input exchange, input delay, state hashing, desync reports, replays and the cross-leg golden-hash CI job can be built and proven on physics-free scenes (transform + Lua + RNG) today. **Rollback is the hardest part, and not mainly because of physics**: this engine's simulation state is spread over heap GameObjects with side-effecting lifecycles, Lua tables and closures, coroutines and the physics backend; a full-world SaveState/RestoreState needs a constrained "rollback world" (section 4.4). Recommendation: **lockstep first, rollback second**.
- **Browser reality:** a static host cannot be a game server, and a browser cannot accept connections. Browser clients reach a native `N2Server` (or a relay) over **WebSocket** (works today in every browser, TCP so loss stalls), **WebRTC data channels** (unreliable, but needs signalling + STUN/TURN and a native WebRTC stack), or **WebTransport** (unreliable datagrams, needs an HTTP/3 server; Safari support uncertain [M]). Recommendation: WebSocket first, measure, then one UDP-like browser transport. The browser tab's main loop also pauses or throttles when hidden [M], which stalls a lockstep session for everyone.
- **Testing:** the engine is a set of process singletons (`Application::GetInstance`, static `SceneManager`, `LuaRuntime::Instance`) [V `Application.cpp:34-38`, `SceneManager.hpp:44-64`, `LuaRuntime.hpp:47,68`], so two peers cannot share one process. Hence: pure-logic tests in-process on the simulator, multi-process integration tests on loopback in CI, cross-platform determinism checked offline by replaying one input log on every CI leg and comparing hashes in a new aggregation job. Live Windows + Linux + browser sessions need the user.
- **Size:** about 18-22 PRs [E] (section 7); the authoritative track is roughly 2/3 of them and none of it blocks on #156.

---

## 1. Goals, non-goals, threat model

### 1.1 Goals
- G1 Two built-in presets beside None/offline: **Server authoritative** (PhysX, prediction + reconciliation) and **Deterministic** (lockstep, then rollback; cross-platform Windows + Linux x86_64/arm64 + web).
- G2 One gameplay API for both where possible: `NetworkObject`, ownership, inputs, RPCs/commands, the same in C++ and Lua.
- G3 Native and browser peers in the same session (D2).
- G4 Everything testable in CI without a second machine, except what only a real network or browser can show.
- G5 Secure by default: encrypted and authenticated on any non-loopback bind, bounded work per peer before and after authentication.

### 1.2 Non-goals (v1)
- Matchmaking, lobbies as a service, accounts, NAT punch-through, a TURN/relay fleet. v1 is "connect to an address with a join token"; the dedicated server is the relay.
- Predicting arbitrary rigid bodies under the Authoritative preset (only owned kinematic movement; section 3.3).
- Anti-cheat beyond server authority and input validation (no client integrity checks).
- Voice, large file transfer, cross-version play (peers must run the same build).
- Determinism with PhysX (ruled out by D2 and measured: Windows and Linux hashes differ for every scene, even free fall [V `physx-determinism-measured-numbers.md:44-46`]).

### 1.3 Threat model
| Threat | Authoritative preset | Deterministic preset | Mitigation |
|---|---|---|---|
| Client sends impossible state | Clients send only inputs and RPC requests; server simulates | Clients send only inputs; every peer simulates | Server validates input ranges and rates; deterministic peers validate inputs identically (same code), so an illegal input is rejected everywhere |
| Speed hack (more inputs per second) | Server consumes at most one input per client per tick, bounded queue | Input slots are per tick; extra inputs dropped | Input queue cap, tick-stamped inputs |
| Information leak (wallhack/maphack) | Interest management limits what is sent | **Inherent**: every peer has the full state | Documented; choose Authoritative for competitive hidden-information games |
| Lag-compensation abuse (fake high latency) | Rewind capped (default 200 ms) | n/a | Cap + use the server's measured RTT, not the client's claim |
| Desync griefing / modified client | n/a | A modified client diverges | State hashes; v1 ends the session with a report naming the first divergent tick (section 4.5) |
| Spoofed source / reflection amplification | UDP handshake | same | Challenge cookie before any state; handshake reply never larger than the request (padded connect request) [M: netcode.io pattern] |
| Connection exhaustion (DoS) | Pre-auth slots, per-address + global token buckets, failed-auth backoff | same | **Reuse** `PreAuthSlots`, `TokenBucket`, `RateLimiter` (move to a shared library) [V they are socket-free policy classes] |
| Expensive messages (CPU DoS) | Bounded decode, no JSON on the wire, RPC handlers run under Lua budgets | same | Per-message size caps, per-RPC rate limits, `LuaLimits.instructionsPerEntry` [V `LuaRuntime.hpp:19-25`] |
| Eavesdropping, tampering, replay of packets | AEAD per packet, nonce = sequence | same | Keys from the authenticated handshake (section 2.1.3) |
| Token theft / reuse | Short-lived join tokens, role in token | same | Token table pattern with roles (player, spectator, admin) |
| Peer IP exposure | Server only | P2P would expose peers | v1 is star topology via the server/relay (N5) |
| Hash flooding of Lua tables | n/a | `determinism.luaSeed` makes the string hash seed constant | Already warned at startup [V `Application.cpp:123-127`]; never put network-controlled strings into sim tables as keys without a cap |

---

## 2. Architecture

```
            +-------------------- engine/src/network (engine layer) ---------------------+
 Lua/C++ -> | NetworkObject, Replicated fields, RPC/commands, InputFrame, NetworkTransform |
            | AuthoritativeRuntime (server | client)      LockstepRuntime (+ rollback)     |
            +--------------------------------------+---------------------------------------+
                                                   | uses
            +------------------- net/ ("n2net", no engine dependency) -------------------+
            | Session/handshake, crypto (AEAD), channels (reliable-ordered, unreliable-   |
            | sequenced), fragmentation, acks, RTT/clock sync, snapshot + input buffers,  |
            | delta encoder, lockstep input scheduler, state-hash tree, replay log        |
            +----------------- ITransport -----------------------------------------------+
              UdpTransport (Win/Linux)  WebSocketTransport (native server + Emscripten client)
              SimTransport (in-memory, seeded impairment)  later: WebRTC or WebTransport
```

### 2.1 Transport abstraction
- `ITransport`: datagram-oriented, non-blocking, polled: `Send(peer, bytes)`, `Poll(callback)`, MTU, `IsReliableStream()` (true for WebSocket so the upper layer skips its own resends but keeps sequencing). One code path for native and web: polled on the main thread at the start of each tick, so the single-threaded web build needs no thread [V web baseline has no threads: `docs/web-build.html:100,148`]. A native I/O thread is an optimisation for later.
- Packet: little-endian, max 1200 bytes [M: safe for IPv6/WebRTC paths], header = protocol id (hash of net protocol version + game build fingerprint), connection id, 16-bit sequence, ack + 32-bit ack bits [M: Gaffer-on-Games style]; payload = channel blocks. Channels: **reliable-ordered** (spawns, RPCs marked reliable, join state), **unreliable-sequenced** (snapshots, inputs; newest wins), fragmentation for large reliable messages (initial state).

#### 2.1.1 Native
- Own UDP layer over BSD/Winsock sockets (no new dependency) vs adopting ENet (MIT, no encryption), Valve GameNetworkingSockets (BSD-3, crypto + relay, heavy deps), yojimbo/netcode (BSD-3, libsodium) [M all]. Recommendation (N2): **own layer**, because browser transports, our security model and the simulator have to be ours anyway, and the reliability layer is the small part [E: ~1500 lines with tests].

#### 2.1.2 Browser (web player is single-threaded, static hosting, no special headers [V `docs/web-build.html:100,226,366`])
| Option | Unreliable? | Server needs | Certificates | Browser support | Fit |
|---|---|---|---|---|---|
| WebSocket | No (TCP: a lost packet stalls everything behind it) | A WebSocket listener in `N2Server` (HTTP upgrade + framing; TLS for `wss://`) | `wss://` from an https page needs a CA-trusted cert (Let's Encrypt or a TLS-terminating proxy); the editor's self-signed TOFU model does not work in a browser [M] | Universal [M] | Lockstep (input streams are small; latency spikes under loss), Authoritative at reduced quality |
| WebRTC data channel | Yes (SCTP unordered, maxRetransmits 0) | Native WebRTC stack (e.g. libdatachannel [M: MPL-2.0]) + a signalling endpoint; STUN; TURN for strict NATs | DTLS with self-signed certs whose fingerprints travel in signalling (no CA needed) [M] | Universal [M] | Best browser quality; heaviest dependency; also enables browser P2P (with signalling) |
| WebTransport | Yes (HTTP/3 datagrams) | HTTP/3/QUIC server (e.g. msquic/quiche) [M] | CA cert, or `serverCertificateHashes` for short-lived self-signed certs [M] | Chromium + Firefox; Safari uncertain in 2026 [M] | Clean API; Safari risk |
Emscripten exposes a WebSocket API (`emscripten/websocket.h`) [M]; WebRTC/WebTransport would be JS glue (`EM_JS`) around browser objects, all non-blocking and callback driven, which suits the polled `ITransport`. A static host serves only the client; the server is always a native process (or a browser peer reachable via WebRTC signalling, which itself needs a server).

#### 2.1.3 Encryption and authentication
- Handshake: client sends a padded connect request carrying a **join token**; server answers a challenge cookie (stateless, HMAC of address + time); client echoes it; only then does the server allocate a slot. Keys: derived per connection (ECDHE in the handshake, or token-carried keys in a netcode.io-style design [M]); packets sealed with AEAD (ChaCha20-Poly1305 or AES-GCM), nonce = packet sequence + direction.
- Library: **Mbed TLS 3.6.7 is already vendored** [V `external/mbedtls/include/mbedtls/build_info.h:37`] with DTLS and the cookie module enabled in its default config [V `mbedtls_config.h:1982,2024,3680`], but it is only built for the editor host behind `N2ENGINE_EDITOR_TLS` (default OFF) [V `editor-server/CMakeLists.txt:42`]. The game needs it in the player and server builds, including the web build (for UDP-less web transports the browser provides TLS/DTLS itself, so the web build can skip it). Option: full DTLS 1.2 via Mbed TLS (standard, heavier handshake, needs per-peer session objects) vs our own AEAD packet layer with an ECDHE handshake using Mbed TLS primitives (lighter, our responsibility to get right). Recommendation: own AEAD layer using Mbed TLS primitives, reviewed against the netcode.io standard [M], with a security review PR of its own.
- Fail closed like the editor host: a non-loopback bind without encryption refuses to start unless `--insecure-bind` [V precedent `docs/logging-and-editor.html:218`].
- Join tokens: v1 = a static tokens file with roles (`player`, `spectator`, `admin`), hashed like the editor's (`TokenTable`, SHA-256 via Mbed TLS) [V `TokenTable.hpp:14-60`], or one shared session password. A signed short-lived token from an external backend is a later option.

#### 2.1.4 Relay and NAT
- v1: client-server only; the dedicated server needs a reachable address (port forward or hosting). Deterministic sessions also run in a **star through `N2Server` in relay mode** (it forwards and timestamps inputs, optionally also simulates to check hashes). No NAT traversal, no TURN in v1 (N12).

### 2.2 Session model
- `NetSession` states: Connecting, Handshaking, Loading (scene + build fingerprint check), Synchronising (clock), Running, Ending. Server/host decides the scene; clients load the same scene file and must report the same **scene content hash** and **build fingerprint** (engine version, net protocol version, hash of the project's scripts and scenes; plus the determinism fingerprint of #157 4.4 in the Deterministic preset: compiler id, architecture, SIMD tier, strict-FP flag, backend id + level, tick rate).
- Roles per connection: player, spectator, admin (kick, change map).
- Disconnect/reconnect: Authoritative: reconnect = rejoin with full state. Deterministic: v1 has no late join (N13), a disconnected player's inputs become "empty" after a timeout or the session ends (game setting).

### 2.3 Authority model
- Authoritative: server owns every `NetworkObject`; an object may have an **owner** connection that may send inputs and RPCs for it; client-side prediction only for owned objects with a predicted movement component.
- Deterministic: no per-object authority; inputs are owned by player slots; every peer simulates everything (#157 4.2 row "Authority rules").

### 2.4 Tick and clock
- Today: `Application::Tick` adds the real frame delta (clamped at 0.25 s, dropping the rest) to an accumulator and runs `PhysicsUpdate` while the accumulator holds a step; the step size is `fixedUnscaled * timeScale`, and with timeScale 0 no step runs at all [V `Application.cpp:380,397,403-411`, `Time.cpp:58-61`]. `ProcessAttachQueue` runs once per frame before the fixed steps, so a component created in `Update` joins the simulation at a frame-timing-dependent tick [V `Application.cpp:401`]. `Update` uses a variable dt [V `Application.cpp:415`]. There is no tick counter [V: none in `Application.hpp:28-148` or `Time.cpp`]. `TickOptions::deltaSeconds` already lets a host step time exactly [V `Application.hpp:24-25`], which the tests will use.
- Proposed (both network presets): a **`SimulationClock`** owned by the network runtime decides how many fixed ticks run in this frame; a 64-bit **tick counter** (`Time::GetTick()`), `Time::GetFixedDeltaTime()` independent of timeScale in network presets (timeScale becomes presentation-only or is refused), attach queue processed at each tick boundary, no time dropped silently (a client too far behind resyncs instead). The None preset keeps today's loop byte-for-byte.
- Clock sync: client estimates RTT and the server tick from ping/pong (median of the last N samples); Authoritative clients run **ahead** of the server by RTT/2 + input jitter buffer, adjusting by small time dilation (e.g. +/-5 % tick rate) instead of jumps [M: Overwatch GDC 2017]; Deterministic peers run at the confirmed-input pace with input delay.

### 2.5 `NetworkObject` and replicated components (fits the existing component/serialization model)
- What exists [V]: components register members with `RegisterMember(name, member)`, which captures a reference to the member and produces JSON serialize/deserialize lambdas plus a `FieldInfo` (`ComponentSerializer.hpp:38-65,150-189`); `FieldInfo` has a `FieldKind` (Bool, Int, Float, String, Vector2/3/4, Quaternion, Color, Enum, AssetRef(List), GameObjectRef(List), ComponentRef(List), Json) and flags `hidden`, `readOnly` (`FieldInfo.hpp:28-96`); `SetEditorFields` applies only the keys present (`Component.hpp:74-78`); Lua components keep declared fields in a JSON `_scriptData` and extract them from the Lua instance (`LuaComponent.hpp:47,110`). Objects and components have UUIDs (`base/Asset.hpp`, 128-bit), but lookup by UUID is a full traversal (`Scene.cpp:346-358`), and instantiating a prefab gives fresh random UUIDs (`Prefab.hpp:38`, `PrefabManager.hpp:21,42`).
- Proposed:
  - `NetworkObject` component (singleton per GameObject): a 32-bit **net id** (server-assigned in Authoritative; deterministic counter `(tick, spawner slot, sequence)` in Deterministic), owner, prefab id (index into a registered-prefab table sorted by name), relevance settings. Scene-placed objects are matched by their scene-file UUID at load (both sides load the same file; scene hash checked) and given net ids in sorted UUID order; spawned objects travel as (prefab id, net id, initial replicated values). A hash map net id -> object replaces UUID traversal.
  - Field replication is **opt-in**: `RegisterMember(...).Replicated(ReplicateTo::All | OwnerOnly | ServerOnly, Quantize{...})` in C++, and `replicated = true` in a Lua field declaration. A new `FieldInfo` flag, so the editor can show it.
  - Wire codecs chosen from `FieldKind`: Bool 1 bit, Int zig-zag varint, Float 32-bit or quantized by range, Vector3 3 floats or quantized, Quaternion smallest-three (3 x 10-15 bits) [M], Enum index, String length-prefixed with a cap, GameObjectRef/ComponentRef as net id (+ component index), AssetRef as an index into a per-session asset table. **`Json` fields are not replicable** (refused by validation), so no JSON on the wire.
  - Read path v1 goes through the existing serialize lambda (JSON per member) and encodes it; cost is acceptable for small worlds [E: ~1-3 us per field per send]; v2 adds a raw-pointer fast path to `MemberSerializer` for POD kinds (the member reference is already captured, `ComponentSerializer.hpp:161,173`).
  - Type ids: component types are numbered from the **sorted** list of registered names, never from registry iteration order (`ComponentRegistry::_creators` is an `unordered_map`, `ComponentRegistry.hpp:66,129-138`) [V].
  - `NetworkTransform`: position/rotation (and optionally velocity) with its own quantisation, interpolation buffer and smoothing; the plain `Transform` is not replicated by itself.
- Delta compression: per client, encode each object's replicated fields and compare with the **last acknowledged baseline** for that client (compare-based dirty detection, no change hooks); send changed fields with a field bitmask; baselines expire after N snapshots (then a full object state is sent).
- Interest management and priority: per-object relevance (always, owner-only, radius from the client's observer object on a uniform grid); per-object priority accumulator (priority x time since last sent) and a per-client byte budget per snapshot, highest accumulators first [M: Tribes/Gaffer "state synchronization" pattern].

### 2.6 RPCs and commands (C++ and Lua)
- C++: `RegisterRpc("Fire", &Gun::Fire, RpcTarget::Server, Delivery::Reliable)` on a component; arguments limited to replicable kinds.
- Lua: declared next to the fields, like `SerializableFields`, so argument types are known before any call and checked on receipt:
  `Rpcs = { Fire = { target = "server", reliable = true, args = { "Vector3", "float" } } }`, called as `self:Rpc("Fire", dir, power)`, handled by `function Gun:OnRpc_Fire(sender, dir, power)`.
- Server-side handlers run under the existing Lua budgets [V `LuaRuntime.hpp:19-25`] and a per-connection, per-RPC token bucket; an RPC from a non-owner to an owner-only target is dropped and logged once.
- Deterministic preset: RPCs that change simulation state are **not allowed**; gameplay commands go into the input stream (`InputFrame` holds sampled actions plus a bounded list of command events) so every peer applies them at the same tick. Cosmetic RPCs (chat, emotes) stay outside the sim.

### 2.7 Inputs
- Today input actions update per frame from window events, and action maps live in an `unordered_map<std::string, ...>` [V `Application.cpp:382-385`, `InputSystem.hpp:33`].
- Proposed: a project-declared **networked input layout** (which actions, their value types and quantisation, in a fixed sorted order) sampled once per tick into an `InputFrame`; sim code reads `InputFrame` for a player slot, not the live devices. That is what crosses the network in both presets, and what replays store.

---

## 3. Server-authoritative runtime (PhysX)

### 3.1 Dedicated/headless server
- What exists [V]: a windowless software-renderer path (`Window::UsesNoWindow`, `Window.cpp:61-64`), a play host that ticks with `TickOptions` (render off, explicit delta) (`EditorServer.cpp:5580-5599`, `Host.cpp:342`), and PhysX on Linux x86_64/arm64. Gaps: headless `Init` opens a loopback audio device for the editor stream [V `Application.cpp:252-275`], and the software renderer object is still created; a server wants neither.
- Proposed `N2Server` executable: `--project`, `--scene`, `--bind/--port` (ready line on stdout like the editor host [V precedent `docs/logging-and-editor.html:299`]), `--tokens-file`, `--max-clients`, `--relay` (deterministic relay mode), physics GPU forced off, no audio, no renderer, Linux service/Docker recipes reused from the editor host docs.

### 3.2 Inputs on the server
- Clients send their last 3-5 unacknowledged `InputFrame`s in every packet (redundancy against loss). The server keeps a small jitter buffer per client (1-3 ticks, adaptive), consumes exactly one input per client per tick, repeats the last input when one is missing (and tells the client which tick it used, for reconciliation).

### 3.3 Client prediction (owned entities only)
- v1 predicts **owned kinematic movement**: a `PredictedMovement`/character-controller component whose step is a pure function of (state, `InputFrame`, dt, scene queries). Replay = rerun that function N times; the rest of the world is not resimulated.
- Why not predicted rigid bodies in v1: replaying a dynamic PhysX body means resimulating the PhysX scene, which steps every body, costs N full steps per correction and is not bit-exact on replay (contact caches differ after a state write) [#157 report row 2, M]. Possible later behind an opt-in, using the set-state API below.
- Engine work: a character controller. Options: expose PhysX's CCT (linked: `PhysXCharacterKinematic_static_64` [V `CMakeLists.txt:213`], unused by the engine) or an engine-owned kinematic controller over backend sweeps (capsule sweep/overlap queries; the interface has `SphereCast` only [V `IPhysicsBackend.hpp:183-205`]). Recommendation (N9): engine-owned over new `IPhysicsBackend::CapsuleCast/Overlap`, so it also runs on a future Jolt/Box2D backend.

### 3.4 Snapshots and interpolation of remote entities
- Server sends snapshots at 20-30 Hz (T), delta-compressed per client. Clients render remote objects at `serverTime - interpolationDelay` (default 2 snapshot intervals + measured jitter), interpolating positions (Hermite with replicated velocity when present) and slerping rotations; extrapolation capped at 250 ms, then freeze.

### 3.5 Reconciliation (rewind/replay) using PhysX set-state
- Client keeps a ring buffer per predicted object: (tick, input, predicted state). On a snapshot for tick T with "last input processed" = T: compare server state with the predicted state at T; within epsilon: drop history <= T. Otherwise: **set** the object to the server state, **replay** inputs T+1..now through the movement function, record the visual error, decay it over ~100 ms (T), snap above 1 m (T) (#157 4.2 rows).
- PhysX calls that exist [V headers]: `PxRigidActor::setGlobalPose` (`PxRigidActor.h:129`), `PxRigidDynamic::setKinematicTarget` (`PxRigidDynamic.h:110`), `setWakeCounter` (`:232`), `putToSleep` (`:275`), `setLinearVelocity` (`:352`), `setAngularVelocity` (`:389`).
- What the engine exposes today [V]: `SetVelocity`, `SetAngularVelocity`, getters for pose and velocities (`IPhysicsBackend.hpp:161-169`); `SetBodyTransform` sets a **kinematic target** on any dynamic actor (`PhysXBackend.cpp:827-834`), and `Rigidbody` ignores transform changes of dynamic bodies (`Rigidbody.cpp:336-338`). So a dynamic body cannot be teleported and its sleep state cannot be read or set through the interface.
- Add: `struct BodyState { position, rotation, linearVelocity, angularVelocity, bool sleeping; float wakeCounter; }`, `GetBodyState`/`SetBodyState` (teleport for dynamic, target for kinematic), `SupportsSetState()` (the S2 item from #157 4.4). PhysX implementation with the calls above.
- Measured context [V `physx-determinism-measured-numbers.md`]: a 1-ulp nudge in a 100-body pile passes 1 mm after 49-59 steps and 1 cm after 63-68 steps at 60 Hz; a 10-box stack stays under 16 um for 10 s. A 6-12-tick prediction window is well inside that, so chaos does not hurt reconciliation; corrections come from real mispredictions (other players' effects), which smoothing hides.
- Bonus: same-binary PhysX was bit-identical run to run and with 0 vs 4 threads [V same file, `:33-46`], so **server-side replays** (kill cams, bug reports) of an Authoritative match are possible on the server's own binary.

### 3.6 Lag compensation
- Server keeps, per lag-compensated object, a history ring of hitbox poses for `lagCompMaxMs` (default 200 ms). A hit query RPC carries the client's render tick (interpolation time); the server clamps it to the cap and to its own RTT estimate, poses the history at that time and tests the query.
- Implementation v1: our own ray/sphere-cast tests against simple hitboxes (spheres, capsules, boxes) in the history, no PhysX needed; this also avoids moving live PhysX actors. Alternative: a separate PhysX scene query system for hitboxes (`PxCreateExternalSceneQuerySystem`, `extensions/PxSceneQuerySystemExt.h:57` [V]).
- Exposed as `Physics.RaycastLagCompensated(...)` (server only) in C++ and Lua.

---

## 4. Deterministic lockstep / rollback runtime

### 4.1 Input-only exchange
- Each player slot's `InputFrame` for tick T + inputDelay is sent with all unconfirmed earlier frames; the relay (star, N5) forwards and confirms. A tick is simulated when every slot's input for it is known (lockstep) or predicted (rollback). Input delay default 2 ticks (T 0-6) (#157 4.2).
- Stall policy: if inputs are missing for longer than a timeout, the session pauses with a "waiting for player" state; a hidden browser tab produces exactly this [M], so the web player should send "paused" keepalives and the game decides (wait, drop, or end).

### 4.2 Rollback window
- Max rollback 8 ticks (T 0-15; 0 = pure lockstep). On a late input that differs from the prediction (default prediction: repeat last input), restore the snapshot at that tick and resimulate to now. Costs up to 8 extra sim ticks in one frame [E: affordable only for small worlds].

### 4.3 What the simulation may not do (the blocker list, updated at `637797c`)
| # | Rule | Status in the engine [V] |
|---|---|---|
| B1 | One SIMD tier on all peers | Setting exists (`math.simdTier`, read at `Application.cpp:141-153`); preset forces Scalar; the web build is scalar anyway [V `docs/web-build.html:141`] |
| B2 | Strict-FP build | `N2ENGINE_STRICT_FP` exists, default OFF (`CMakeLists.txt:32-61`); preset requires it (S: `N2_STRICT_FP`/`Math::IsStrictFp()`), Release-only peers |
| B3 | No hash- or pointer-ordered iteration in sim | Coroutines now ordered (`CoroutineScheduler.hpp:25-34`, PR #160); input maps still `unordered_map<string>` (`InputSystem.hpp:33`): sim reads the sorted `InputFrame`, not the maps; component type ids sorted (2.5) |
| B4 | Lua: fixed hash seed, no `pairs()` order dependence, no libm, no `__gc` side effects | Seed switch exists (`determinism.luaSeed`, `Application.cpp:115-127`, PR #172); ordered-iteration helper (D7) not written; tables keyed by tables/functions/userdata hash by address [M Lua 5.4 `ltable.c`], so even a fixed seed does not order them; `math.sin` etc. are C libm |
| B5 | Tick-owned loop, timeScale-independent dt, spawns at tick boundaries, sim only in fixed ticks | Not done (2.4) |
| B6 | Only the session-seeded RNG | `Random.Seed` switches `Random.*` to `DeterministicRandom` (`math/include/math/Random.hpp:12-43`, `LuaMathBindings.cpp:35-37`); preset seeds it from the session and refuses an unseeded call in sim |
| B7 | No thread-order dependence | Physics backend must be thread-count independent or single-threaded |
| B8 | Engine-owned transcendental functions | **Not done**: `Quaternion.cpp`/`Matrix.hpp` still call `std::sin/cos/acos/atan2` (#157 B8); no engine trig in `math/include/math/` [V file list] |
| B9 | Stable spawn order and **deterministic ids** | Spawns get random UUIDs (`Prefab.hpp:38`); the net id counter (2.5) replaces UUIDs as the sim identity |
| B10 | Deterministic physics backend | None; PhysX ruled out by D2; #156 |
| B11 (new) | No wall-clock reads in sim | `Time::GetTime()` accumulates real frame deltas (`Time.cpp:105-112`); sim uses `tick * fixedDt` |
| B12 (new) | Cosmetic systems stay out of sim | GPU particles, soft bodies (`CpuSoftBodyBackend` steps in creation order but is unproven cross-platform), audio, UI layout: may read sim state, never write it |
| B13 (new) | No NaN in sim state (hashing and NaN bit patterns differ across platforms [M]) | Hash canonicalises NaN and the desync report flags it |
| B14 (new) | Float parsing/printing of scene data identical on all peers | nlohmann::json parse/print of floats [M: round-trips exactly with its own dtoa]; verified by the cross-leg scene-hash test |

### 4.4 Full-world SaveState/RestoreState (rollback only)
- What a snapshot must contain: the physics world (body states + solver caches or a backend that has none to restore, e.g. Jolt `SaveState/RestoreState` through `StateRecorder` [M], Box2D v3 [M: no built-in snapshot; needs copy of world]); replicated fields of every `NetworkObject`; the set of live net ids (spawns and despawns inside the window); RNG stream state (`DeterministicRandom` is one u32 [V `DeterministicRandom.hpp:24-30`]); the tick counter; the sim part of Lua state; pending tick-boundary attach/destroy queues.
- Why the engine cannot do this generically: GameObjects are heap objects with lifecycles that have side effects (OnAttach creates PhysX bodies and audio sources, OnDestroy releases them) [V `Rigidbody::OnAttach` creates bodies per the #157 report B9]; `Scene::Serialize` is JSON [V `Scene.hpp:175-178`], so a restore by reload would rebuild bodies cold and take milliseconds [E]; Lua script state lives in Lua tables, upvalues and coroutines, not in `_scriptData`.
- Proposed "rollback world" contract:
  1. Sim state = replicated fields + `NetworkTransform` + physics body state + RNG + net id set. Anything else is presentation and is rebuilt from sim state.
  2. Lua sim scripts keep cross-tick state **only in declared fields** (`replicated = true`); on restore, fields are pushed back into the Lua instance. Locals, closures and coroutines must not carry sim state across ticks. Not statically enforceable; the desync detector catches violations (N7).
  3. Spawns/despawns inside the window: despawned objects are deactivated and parked (no OnDestroy) until their tick is confirmed; spawns after a restore point are parked again on rollback; the pool reuses them on resimulation.
  4. Sim-triggered effects (sound, particles) are **rollback-aware events** keyed by (tick, net id, event id), played once and cancelled if a rollback removes them [M: standard in GGPO-style engines].
- Cost [E]: this is the largest single item (2-3 PRs) and blocks on #156 for the physics part.

### 4.5 State hashing, desync detection, diagnostics
- Every N ticks (default 30, #157 4.2) each peer hashes the confirmed sim state: a **hash tree** (tick -> per net id -> per field), leaves over the encoded bytes (the same codecs as 2.5, so hashing reuses replication code), 64-bit (xxHash/FNV-1a [M], no Mbed TLS needed). Peers exchange the root; the relay compares.
- On mismatch: v1 stops the session (N14) and both peers write a **desync report**: build + determinism fingerprints, the input log from tick 0, the hash trees of the last K checkpoints, and the first divergent net id + field, found by exchanging subtree hashes. The input log is a **replay file**: re-running it headless on each platform pinpoints the diverging tick offline.
- Replays double as the CI golden tests (section 6).

### 4.6 Spectators and late join
- Spectators receive confirmed inputs (delayed) and simulate; a late joiner or spectator needs a full sim-state transfer at tick T plus inputs since T, which is the rollback snapshot serialised. v1 (lockstep only): spectators from tick 0 only, no late join (N13).

### 4.7 How Lua sim code is constrained (D7 included)
- Deterministic preset installs at Lua state creation: fixed `luaSeed`; `math.random` seeded from the session (exists via the seed switch); `math.sin/cos/tan/asin/acos/atan/exp/log/pow` replaced by engine-owned implementations (needs B8); `os.time/os.clock` absent (the game state opens only base, math, string, table [V `engine/src/scripting/LuaRuntime.cpp:264-269`]); an **ordered iteration helper** `Ordered.pairs(t)` (sorted keys: integers, then strings; other key types are an error in sim); a debug-mode lint that warns on `pairs()` in functions called from `OnFixedUpdate`/`OnNetworkTick` [E: heuristic only].
- Sim callbacks: `OnNetworkTick(tick, inputs)` (or `OnFixedUpdate` in a network preset); `Update`/`LateUpdate` are presentation-only by documentation, checked by the desync detector.

### 4.8 Strict-float vs fixed-point (options for #156, not decided here)
| | Strict float (IEEE binary32/64, no contraction, own trig) | Fixed point (e.g. Q32.32 or Q48.16 in int64) |
|---|---|---|
| Math library | Keep `Vector3`/`Quaternion` (float). Needs B1 scalar tier (later: bit-exact SIMD tiers verified by exact-equality tests), B8 own trig/exp/log/pow, no FMA. Small to medium. | New `Fixed`, `FixedVector3`, `FixedQuaternion`, matrices; 64x64->128 multiply (`_mul128`/`__int128`; wasm has i64 [M]); duplicate sim math beside the float render math. Large. |
| Physics | Jolt with `CROSS_PLATFORM_DETERMINISTIC` (3D) or Box2D v3 (2D) claim cross-platform determinism [M, #156 verifies] | No maintained fixed-point Jolt/Box2D [M]: a custom engine. Very large. |
| Lua | Numbers are doubles; strict-FP build + own math functions suffice | Fixed values as userdata (slow, awkward) or raw integers (unergonomic) |
| Serialization/editor | Unchanged | New `FieldKind`s, inspector conversion |
| Guarantee | Depends on compilers honouring IEEE without contraction; NaN payloads, denormal modes (FTZ/DAZ set by any library), libm, constant folding of transcendental calls are the risks [M]; guarded by cross-leg golden hashes on every toolchain bump | Bit-exact by construction (integer ops), independent of compiler flags |
| Precision/range | Float everywhere; large worlds need origin shifting either way | Range/precision chosen per game; overflow is a new bug class |
| Web | wasm floats are IEEE, no FMA unless relaxed-SIMD [M]; arguably the easiest platform | Fine |
| Cost [E] | Weeks (B8 + backend integration) | Months (math + physics + bindings) |

---

## 5. Presets, settings, validation, editor

### 5.1 Settings model
The `network` block of #157 4.1 in the project file's settings (parsed next to `physics`, `math`, `determinism` in `ApplyProjectSettings` [V `ProjectSettings.cpp:104-196`]; values needed before Init are read in `Application::Init` like `physics.deterministic` [V `Application.cpp:202-219`]):
```json
"network": {
  "preset": "none | authoritative | deterministic",
  "tickRate": 60,
  "transport": { "udp": true, "webSocket": true, "port": 7777 },
  "authoritative": { "snapshotRate": 30, "interpolationDelayMs": 100, "inputRedundancy": 4,
                     "predictionMaxMs": 250, "smoothingMs": 100, "snapThreshold": 1.0, "lagCompMaxMs": 200 },
  "deterministic": { "mode": "lockstep | rollback", "inputDelay": 2, "rollbackWindow": 8, "hashInterval": 30 },
  "overrides": {}
}
```
The preset table (#157 4.2) stays the source of locked (L) / tunable (T) / startup-checked (S) rows; this design adds the transport and runtime rows above.

### 5.2 Startup validation
- `IPhysicsBackend::GetDeterminism()` (`None | SameBinary | CrossPlatform`), `SupportsSnapshot()`, `SupportsSetState()` (#157 4.4). PhysX: `None` (or `SameBinary` with `physics.deterministic`, as measured), `SupportsSetState() = true` once 3.5 lands, `SupportsSnapshot() = false`.
- Deterministic preset: **refuse to start** (D4) unless backend `CrossPlatform`, strict-FP build, Scalar tier forced, fixed `luaSeed`, engine trig present, physics threads independent, GPU off. Authoritative: warnings only (GPU physics on a server, `SupportsSetState` false).
- A `Network` row in `EngineHealth` (beside the seven rows of `Application.cpp:307-308`) with the preset, transport state and, for Deterministic, the unmet requirements.

### 5.3 Editor (as #157 4.3, plus)
- Network panel cards: None, Server authoritative, Deterministic (disabled with "Requires a deterministic physics backend (none installed)" per D3).
- Nudge when the first `NetworkObject` is added with preset `none` (`AddComponent`, `protocol.json:580` [V]); `ComponentSchema` gains a `network` tag so the editor knows which types count.
- Validation report through `GetEngineHealth` (`protocol.json:557`) and changes through `SetProjectSettings` (`:660`) [V]; a minor protocol bump (currently 1.13.0 [V `Protocol.hpp:14`]; the Lua editor extensions also plan 1.14.0, so whichever lands second takes the next minor).
- Play-in-editor with networking (server + N clients locally) is **not** in v1: the play host is one process with one engine; v1 launches `N2Server` + extra player processes from the editor as a later item.

### 5.4 Per-preset testing story (what a game author gets)
- None: unchanged.
- Authoritative: a "local session" launcher (server + 2 clients on loopback) with simulated latency/loss from the settings (the `SimTransport` impairment layer wraps real sockets too), a network stats overlay (RTT, loss, bandwidth, corrections per second).
- Deterministic: record/replay of every session; "desync test" mode that runs two simulations in two processes with identical inputs and compares hashes every tick; cross-platform check by replaying a recorded file on another OS.

---

## 6. Testing strategy

| Layer | What | Where it runs | Verifies |
|---|---|---|---|
| n2net unit tests | Packet codec, acks, reliable channel under loss/reorder/duplication, fragmentation, AEAD seal/open, handshake (cookie, amplification bound), token buckets, clock sync convergence, delta encoder, lockstep scheduler, hash tree bisection | In-process on **`SimTransport`** (seeded RNG for latency/jitter/loss/reorder/dup/bandwidth, virtual clock, no sleeps), every CI leg incl. web (node) | Logic; reproducible from the seed |
| Engine net tests | `NetworkObject` ids, field codecs per `FieldKind`, replication of a scene to a fake client sink, Lua RPC declaration validation, `BodyState` set/get on PhysX, reconciliation math on a scripted movement function, lag-comp history | `engine_tests` (one engine per process) with the other side faked | Engine layer |
| Multi-process integration | `N2Server` + 2 headless clients on loopback UDP (and WebSocket), impairment on, 30 s scripted session; assert convergence, bandwidth budget, no crash | New CI step on Linux x86_64 (and Windows) [E: ~1-2 min] | Real sockets, real processes |
| Golden determinism (cross-leg) | A fixed scene + input log replayed headless for N ticks, printing a hash every 60 ticks; a new **aggregation job** downloads each leg's hashes (Windows Release, Linux x86_64, Linux arm64, web/node) and asserts equality. Starts with physics-free scenes (transforms, Lua sim script, RNG): a canary for B2/B4/B8 now; adds physics scenes when #156 lands | Today the legs are independent jobs [V `.github/workflows/tests.yml:98-106,343-345,468-472,788-792`] and determinism numbers were compared by reading logs; artifact upload/download is new | Cross-platform determinism without a second machine |
| Soak | Server + 8 bot clients, 30 min, impairment, memory/bandwidth/desync counters | `workflow_dispatch`/nightly, not per PR (CI time) | Leaks, drift |
| User runs | Windows + Linux + browser over LAN and internet; NAT; hidden tab; Safari/Firefox/Chrome; TLS certs for `wss://`; real latency feel; 8-player stress | User's machines | What CI cannot reach (GitHub runners cannot connect to each other directly [M]) |

---

## 7. Docs impact and staged PR plan

### 7.1 Docs
New `docs/networking.html` (concepts, presets, NetworkObject, replication, RPCs, prediction, lockstep, testing, security, hosting `N2Server`), `docs/networking-security.html` or a section (threat model, tokens, encryption, fail-closed bind); updates to `physics.html` (BodyState, character controller, lag comp), `scripting-lua.html` (replicated fields, Rpcs, deterministic Lua rules, `Ordered.pairs`), `application.html` (tick ownership, `Time::GetTick`), `web-build.html` (browser transports, hidden tab), `testing.html` (simulator, golden hashes, soak), `editor.html` (Network panel), `serialization.html` (`.Replicated()`), `index.html` design-decision table.

### 7.2 Staged plan (sizes [E]: S < 400 changed lines, M 400-1200, L > 1200; every PR CI-verifiable unless marked)
Track A: foundations (no #156 dependency)
| PR | Content | Size | CI verifies | Needs the user |
|---|---|---|---|---|
| A1 | `network` settings block (parse/validate), preset table as data, `IPhysicsBackend::GetDeterminism/SupportsSnapshot/SupportsSetState`, Deterministic startup refusal, `Network` health row (= the #157 S2 preset items) | M | Parsing, refusal, health row | Accept preset rows |
| A2 | Tick ownership: `SimulationClock`, `Time::GetTick`, timeScale-independent dt and tick-boundary attach queue in network presets; None preset unchanged (golden test that the None loop is identical) | M | Stepped with `deltaSeconds` | - |
| A3 | n2net core: `ITransport`, `SimTransport`, packet header, acks, reliable + unreliable channels, fragmentation | L | Unit tests on all legs incl. web | - |
| A4 | Native `UdpTransport` (Win/Linux), loopback integration test, `--insecure-bind`-style refusal | M | Loopback test | - |
| A5 | Handshake + AEAD (Mbed TLS primitives into the game/server build), join tokens, shared security policy lib (move `TokenBucket`/`RateLimiter`/`PreAuthSlots`/`TokenTable` out of editor-server) | L | Unit + loopback; amplification bound test | **Security review accept** |
| A6 | `N2Server` executable (headless, no audio/renderer, ready line, tokens) | M | Smoke start/stop in CI | Real deployment |
Track B: server-authoritative runtime (PhysX)
| B1 | `NetworkObject`, net ids, prefab table, scene-object matching, spawn/despawn replication, scene + build fingerprint check | L | Multi-process loopback | - |
| B2 | Field replication: `.Replicated()`, Lua `replicated = true`, `FieldKind` codecs, delta vs acked baseline, `NetworkTransform` | L | Codecs, deltas, loopback convergence | - |
| B3 | Clock sync + snapshot interpolation of remote entities | M | Simulator tests | Feel (user) |
| B4 | Networked input layout + `InputFrame`, redundant client inputs, server jitter buffer | M | Simulator tests | - |
| B5 | `BodyState` get/set on `IPhysicsBackend` + PhysX (teleport, sleep), capsule cast/overlap, engine-owned character controller | L | PhysX legs | - |
| B6 | Client prediction + reconciliation + smoothing for the controller | M | Scripted-latency integration test | Feel (user) |
| B7 | RPCs (C++ and Lua declared), budgets, rate limits | M | Unit + loopback | - |
| B8 | Lag compensation (hitbox history, rewound queries) | M | Unit | - |
| B9 | Interest management, priorities, bandwidth budget | M | Simulator | - |
| B10 | Browser: WebSocket transport (server listener + Emscripten client) | M | node client vs native server in the web job [E feasible] | **Real browsers, certs** |
| B11 | Editor Network panel, nudge, validation report (engine PR + frontend PR, protocol minor bump) | M + M | DOM tests (frontend CI) | Live editor (#149) |
Track C: deterministic runtime (C1-C3 not blocked; C4+ blocked on #156)
| C1 | Engine-owned trig/exp/log (B8 of #157) + Lua deterministic shims + `Ordered.pairs` (D7) | M | Exact-value tests on all legs | - |
| C2 | Cross-leg golden-hash job (artifact aggregation) with physics-free replay scenes | M | Itself | - |
| C3 | Lockstep runtime: input scheduling + delay, relay mode in `N2Server`, hash tree + desync report, replay files | L | Simulator + multi-process; golden replays | - |
| C4 | **#156 result**: deterministic backend behind `IPhysicsBackend` declaring `CrossPlatform` (+ snapshot) | L+ | Golden physics hashes across legs | #156 decision |
| C5 | Rollback: world SaveState/RestoreState, parked spawns/despawns, rollback-aware events, input prediction | L | Rollback-vs-lockstep equality tests | - |
| C6 | Spectators + late join via state transfer | M | Integration | - |
| C7 | Second browser transport (WebRTC or WebTransport, per N3) | L | Partly (node) | **Real browsers, NAT** |

Ordering: A1-A2 first (they also unblock #157 S2), A3-A5 in parallel with C1-C2; then B1-B11 and C3; C4 when #156 decides; C5-C6 after C4. Totals [E]: Track A 6 PRs, B 11-12, C 7; ~25 PRs; the first playable authoritative session (two native clients, interpolation + prediction) after A1-A6 + B1-B6 (~12 PRs).

---

## 8. Risks and open questions
- R1 **Rollback scope**: the "rollback world" contract (4.4) restricts how games are written (state only in declared fields, parked spawns). If it proves too restrictive, rollback may be limited to C++ sim components. Open until C5.
- R2 **Strict float across MSVC/Clang/Emscripten** may still diverge (denormal flags set by a library, NaN payloads, constant folding); only the golden-hash job will tell, and every toolchain bump can break it.
- R3 **Browser main loop pauses when hidden** [M]; lockstep sessions stall, authoritative clients time out. Mitigations (keepalive from a Worker, pause protocol) are untested.
- R4 **wss certificates**: a self-hosted `N2Server` needs a CA-trusted certificate for browser clients; the editor's self-signed TOFU model does not transfer [M].
- R5 **Singleton engine**: no in-process multi-peer tests; integration needs processes (slower CI, flakier timing). Making the engine instantiable is out of scope.
- R6 **Replication cost via JSON lambdas** for larger worlds; the POD fast path (2.5) may be needed earlier than planned.
- R7 **PhysX on web** (stage 5 in flight): the authoritative web client predicts only kinematic movement and needs scene queries, so a wasm PhysX (or a query-only backend) is needed for prediction in the browser; interpolation-only web clients need no physics.
- R8 **Security of a custom AEAD protocol**: must be reviewed like the editor host's work; DTLS is the conservative fallback.
- R9 **Input layout as a project asset**: ties networking to the input system's design (rebinding, gamepads); unclear how dynamic action maps fit.
- Open: tick rate per preset defaults (60 for both?), maximum players for v1 (8 proposed for lockstep, 32 for authoritative [E]), whether `Update` should be forbidden from writing replicated fields (a debug assert?), naming (`NetworkObject` vs `NetworkIdentity`).

## Could not verify
- Every [M] claim about third-party libraries (ENet, GameNetworkingSockets, yojimbo/netcode.io, libdatachannel and its licence, msquic/quiche), browser API support (WebTransport in Safari, `serverCertificateHashes`, WebRTC unreliable channel behaviour), hidden-tab throttling details, and Emscripten's WebSocket API surface: none were checked against their docs here.
- Jolt `CROSS_PLATFORM_DETERMINISTIC`/`StateRecorder` and Box2D v3 determinism and snapshot support (#156 verifies; also F2 of the #157 report).
- Whether wasm, x86-64 and arm64 builds produce identical floats for this codebase under `N2ENGINE_STRICT_FP` (no measurement exists; C2 measures it).
- nlohmann::json float round-trip exactness across MSVC/Clang/Emscripten (B14).
- Whether the headless windowless path can skip creating the software renderer without other code depending on it (not traced).
- Lua 5.4's table hashing of table/function/userdata keys by address (B4) is from memory of `ltable.c`, not re-read in `external/lua`.
- GitHub Actions runner-to-runner connectivity (assumed impossible without tunnels).
- All size and cost estimates [E].

## Decisions for the user
- **N1** Build order: the server-authoritative (PhysX) track first, with the non-physics deterministic pieces (C1-C3) in parallel and rollback only after #156? Recommendation: yes.
- **N2** Native transport: our own small UDP reliability layer (no new dependency) rather than adopting ENet, GameNetworkingSockets or yojimbo? Recommendation: own layer.
- **N3** Browser transport for v1: WebSocket only, with WebRTC or WebTransport as a later second transport chosen after measurement? Recommendation: yes, WebSocket first.
- **N4** Encryption and auth mandatory on any non-loopback bind (fail closed, `--insecure-bind` to override), using our own AEAD packet layer on the vendored Mbed TLS, with a dedicated security review PR? Recommendation: yes.
- **N5** Deterministic topology for v1: star through `N2Server` in relay mode (no P2P mesh, no NAT traversal)? Recommendation: yes.
- **N6** Ship deterministic lockstep (with input delay) before rollback? Recommendation: yes.
- **N7** Lua in deterministic sim code: allowed under the rules of 4.7, with rollback-relevant Lua state only in declared replicated fields, enforced by the desync detector rather than static analysis? Recommendation: yes.
- **N8** Authoritative prediction v1 limited to owned kinematic/character movement (no predicted dynamic rigid bodies)? Recommendation: yes.
- **N9** Character controller: engine-owned kinematic controller over new backend capsule casts, rather than exposing PhysX's CCT? Recommendation: engine-owned.
- **N10** Replication is opt-in per field (`.Replicated()`, `replicated = true`), not every serialized field of a networked object? Recommendation: opt-in.
- **N11** A new `N2Server` executable for dedicated servers and relays, rather than a mode of `N2EditorHost` or `lua_project`? Recommendation: new executable.
- **N12** Out of scope for v1: matchmaking, lobby service, NAT punch-through, TURN; v1 is direct connect with a join token? Recommendation: yes.
- **N13** Deterministic v1 without late join (spectators only from the start) until rollback state serialisation exists? Recommendation: yes.
- **N14** On a detected desync, v1 ends the session and writes a desync report + replay (no automatic resync)? Recommendation: yes.
- **N15** Add the cross-leg golden-hash aggregation job to CI now (physics-free scenes), as a non-required check until #156 lands? Recommendation: yes, non-required at first.


## 9. C5 follow-up design notes (2026-10-28, assessed, not implemented)

These three came out of the C5 remainder (#257). Each is a design decision with a protocol or hash consequence, so none was guessed in code.

### 9.1 Player join after the start, and rejoin of a dropped slot

Today only spectators join late (wire version 2). A player needs two things a spectator does not: an input slot and an input history the others agree on.

- **Option A, slot reservation with a join tick.** The relay keeps the slot of a dropped player marked "reclaimable". A rejoiner sends `Rejoin(slot, token)`, the relay picks a join tick T (confirmed tick + inputDelay + margin), tells every peer `SlotRejoin(slot, T)` on the reliable channel, and asks a live player for a snapshot of a confirmed tick at or before T (the spectator machinery). Every peer treats the slot as dropped (synthesised input) up to T and as live from T on; the rejoiner restores the snapshot and runs confirmed ticks up to T on all inputs the relay has kept (kSpectatorHistoryTicks), then submits its own input from T. DropSlot already means "synthesised input from tick X" on every peer, so SlotRejoin is its mirror image. Needs a lockstep wire version 3 and a new message pair; the hash tree is unchanged.
- **Option B, join only at agreed checkpoints.** Same, but T must be a hash checkpoint and the snapshot is taken exactly at it, so the rejoiner's first root is comparable at once. Simpler to verify, longer wait for the player (up to hashInterval ticks, 0.5 s at the default).
- **Option C, no player join; spectator-to-player promotion only.** A spectator that already runs in step asks to take a free slot at tick T. Smallest change (the snapshot is already in step), the same wire messages as A for the slot part.

Recommendation: C then A. A spectator that is already in step removes the snapshot problem, and the slot-switch message (`SlotRejoin`) is the same one A needs, so C is the first half of A. New players joining a session whose slot count was never reached (a lobby) already works. Open question for the user: should a rejoin keep the same slot (and so the same avatar) or take any free one? Recommendation: same slot, matched by the join token's user name, which the relay already sees.

### 9.2 Lua state beyond the declared replicated fields

Today a snapshot holds a Lua script's `replicated = true` fields only; locals, upvalues and coroutines diverge after a rollback (test `RollbackLuaTest`, pinned both ways) and only the desync detector notices.

- **Option A, keep the contract** (N7 as decided): rollback-relevant Lua state must be a declared replicated field. Add a lint at scene load (a script that is in a rollback project and has a module-level local assigned in `OnFixedUpdate`) as a warning. Cheap, no engine risk.
- **Option B, snapshot the script's `self` table** (all non-function, non-userdata fields, deep copy of plain tables). Covers the usual "I keep a timer in self.timer" case without a declaration. Cost: a deep copy per script per tick of the window; needs a stable serialisation order (D7's ordered iteration) and cannot cover upvalues.
- **Option C, snapshot the Lua interpreter** (a custom allocator and a heap copy). Not feasible with the vendored Lua and `lua_State` pointers held by bindings.

Recommendation: A now (a warning plus the documented scope), B only if games ask for it, as an opt-in `rollbackState = true` on a script field table so nothing is copied that the author did not name. C is rejected.

### 9.3 The spectator fallback when the joiner backend refuses the physics bytes

Today `RestoreForJoin` falls back to per-object body states (`exactPhysics` false), the spectator reports no roots and may drift undetected.

- **Option A, refuse the join** (`NoSnapshot` or a new `PhysicsMismatch` refusal): a spectator never runs a view that may be wrong. Honest and trivial; the cost is that a spectator whose scene differs (bodies of non-networked objects created in a different order) cannot watch at all.
- **Option B, keep the fallback but verify it**: the spectator reports roots for the networked-object part of the tree only (a tree without the physics leaf) so a drift is still detected by the relay against a player's own such root. Needs players to send a second, physics-free root (a wire change: a second root per checkpoint) or the relay to hold two trees.
- **Option C, make the bodies match**: have the joining host build its scene through the same ordered creation path as the players (RestoreForJoin already instantiates spawned objects under their ids), so the Jolt bytes apply. This is the real fix and removes the cause for the common case.

Recommendation: C as the goal, A as the interim for a spectator whose restore was not exact (an `exactPhysics` false result ends the session as Failed with a clear message, unless the host sets `allowInexactPhysics`). It makes the unsafe case opt-in instead of silent, with no wire change.
