import fs from 'node:fs'
const D = 'C:/Method Notes Obsidian/orchestrator/items/'
const PR = 'C:/Method Notes Obsidian/orchestrator/proposals/'
const src = fs.readFileSync('C:/Method Notes Obsidian/orchestrator/tools/mk-batch-1022.mjs', 'utf8')
const COMMON = eval(src.match(/const COMMON = (' .*?')\n/s)[1])
const items = []
const add = (o) => items.push({ closes: null, allowProtocol: false, gate: null, approved: true, requiresDocs: true, repo: 'engine', ...o })
const NETPATHS = ['^engine/', '^net/', '^tests/', '^docs/', '^CMakeLists\\.txt$', '^cmake/', '^\\.github/', '^tools/', '^n2server/', '^apps/', '^server/', '^lua_project/', '^[^/]*\\.lua$']
const READFIRST = `Design: ${PR}netcode-runtime-design.md (N1-N15 ACCEPTED): read the whole document, the sections for this stage and the stage table row, then the MERGED code: n2net (transports, SecureTransport, channels, the network simulator and its tests), N2Server, B1 (NetworkObject, world/server/client classes, fingerprint) and B2 (field replication: .Replicated(), codecs, delta vs acked baseline, NetworkTransform) in engine/src/network, A2 (simulation clock, fixed tick, StepTicks, TickOptions), and the existing network docs page. Follow the security rules of the design (bounded counts and sizes before allocation, rate limits, authority checks, no client-controlled allocation). The web build must still compile (n2net core is compiled for web; native sockets are not). Nothing builds locally: re-read every edit twice and trace identifiers.`

add({
  name: 'net-b3-clock-interpolation',
  branch: 'feat-net-b3-clock-interpolation',
  title: 'Netcode B3: clock sync and snapshot interpolation of remote entities',
  issue: `${READFIRST} SCOPE (stage B3 only): (1) client/server clock synchronisation over the existing secure channel (ping/pong style samples with the server tick and a monotonic client clock; filtered offset and RTT estimate using the best-of-N or a median filter, drift handling, never jumps backwards; bounded rates, malformed input rejected; estimates exposed to C++ and Lua as server time/tick, RTT, jitter); (2) snapshot interpolation of remote entities: the client buffers received NetworkTransform states per object with their server ticks, renders remote objects at (estimated server time - interpolation delay) interpolating position/scale linearly and rotation by slerp, with a configurable delay (default from settings: a few snapshot intervals), extrapolation limited and clamped when a snapshot is late, teleport flag respected (no interpolation across a teleport), buffer bounded (no unbounded growth under a flood or long stalls), out-of-order and duplicate snapshots handled; the locally controlled object (owner/prediction) is NOT interpolated (that is B6): provide the hook; (3) tests on the network simulator (latency, jitter, loss, reorder): clock estimate converges and stays within bound; interpolated position is smooth (bounded second difference) under jitter; teleport; late snapshot extrapolation clamp; buffer bounds; deterministic given a seeded simulator; (4) docs: the clock/interpolation section of the netcode docs and scripting-lua.html for the Lua accessors, true against the code.${COMMON}`,
  allowedPaths: NETPATHS,
  acceptance: ['Clock sync converges, never goes backwards, bounded rates; exposed to C++ and Lua', 'Snapshot interpolation with delay, bounded extrapolation, teleport, bounded buffers', 'Simulator tests incl. jitter/loss/reorder; docs true; CI green'],
  reviewer: 'opus',
  notes: 'Reviewer: monotonic clock estimator, no division by zero or NaN at startup (first samples), buffer and rate bounds, tick wraparound handling (u32 ticks), slerp of non-normalised or NaN rotations, interpolation never applied to the locally controlled object, compile-level correctness, web build.',
})

add({
  name: 'net-b4-input-frames',
  branch: 'feat-net-b4-input-frames',
  title: 'Netcode B4: networked input layout, InputFrame, redundant client inputs, server jitter buffer',
  issue: `${READFIRST} SCOPE (stage B4 only): (1) an input layout declaration (a small schema of named axes/buttons/vectors with bit widths and quantisation, declared in C++ and from Lua, registered in the fingerprint from B1 so client and server must agree) and the InputFrame type (tick, sequence, packed payload); (2) client side: sample the local input each fixed tick into an InputFrame, keep a bounded ring of unacked frames, send each datagram with the last N frames (redundancy, N from settings) on the unreliable channel so loss does not lose inputs, acks from the server trim the ring; (3) server side: per client jitter buffer keyed by tick (bounded depth), duplicate/old/too-far-future frames rejected, a configurable input delay target, behaviour when an input is missing (repeat last input N ticks then neutral input; counters exposed), rate limit and size cap per client per second, authority: input is applied ONLY for the client's own controlled object(s) as assigned by the server; (4) a server API for the game to read the input of the controlled object for the current tick (C++ and Lua), deterministic given the same frames; (5) tests on the simulator: loss and reorder do not drop inputs thanks to redundancy, jitter buffer ordering and bounds, flood rejected, missing input policy, quantisation round trip, determinism of encode; (6) docs true against the code. NO prediction/reconciliation here (B6).${COMMON}`,
  allowedPaths: NETPATHS,
  acceptance: ['Input layout schema in the fingerprint; InputFrame encode/decode bounded', 'Redundant sends and acks; server jitter buffer with bounds and missing-input policy', 'Input applied only to the client own controlled object; rate/size limits', 'Simulator tests, docs true, CI green'],
  reviewer: 'opus',
  notes: 'Reviewer (security): every count/size bounded, clients cannot drive objects they do not control, ring/jitter-buffer memory bounded under flood, tick wraparound, quantisation round trip, Lua layout validation, compile-level correctness, web build.',
})

add({
  name: 'net-b7-rpcs',
  branch: 'feat-net-b7-rpcs',
  title: 'Netcode B7: RPCs declared in C++ and Lua, budgets and rate limits',
  issue: `${READFIRST} SCOPE (stage B7 only): (1) RPC declaration: C++ (a registration API next to component registration with a stable id, direction ServerToClient / ClientToServer / ServerToOwner, reliable or unreliable, an argument schema built from the FieldKind codecs of B2) and Lua (declared in the component declaration table the same way fields are, with params typed by the same kind rules as GetLuaFields); RPC ids are part of the fingerprint from B1/B2; (2) sending: calling the RPC on the sending side encodes and queues it on the right channel (reliable ordered or unreliable), with the target (all, one client, owner); receiving: decode with bounds, validate the argument values with ValidateFieldValue/ClampFieldValue, check direction and authority (a client can call only ClientToServer RPCs on objects it owns/controls; the server never executes a ServerToClient RPC received from a client), invoke the handler in the deterministic tick order; (3) budgets and rate limits: per-client RPC per second and byte budgets per RPC id and in total, queue bounds, over-budget RPCs dropped with counters and a throttled log (never unbounded), handler time budget through the existing Lua budget mechanism, a faulting handler does not kill the server; (4) tests on the simulator and loopback: delivery semantics (reliable ordered vs unreliable), authority rejection, argument validation, rate limiting, malformed input, Lua and C++ RPCs; (5) docs true against the code.${COMMON}`,
  allowedPaths: NETPATHS,
  acceptance: ['RPC declaration in C++ and Lua with ids in the fingerprint', 'Direction/authority checks, validated arguments, bounded decode', 'Per-client budgets and rate limits with counters; faulting handler isolated', 'Simulator/loopback tests, docs true, CI green'],
  reviewer: 'opus',
  notes: 'Reviewer (security): authority/direction enforced on the receiving side for every path, argument validation before invoking Lua/C++, no unbounded queues, rate limit state bounded per client, reliable queue cannot exhaust memory, handler faults isolated, ids collision/ordering, compile-level correctness, web build.',
})

for (const it of items) fs.writeFileSync(D + it.name + '.json', JSON.stringify(it, null, 2) + '\n')
console.log(items.map(i => i.name).join('\n'))
