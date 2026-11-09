// The WebGPU soft-body backend's simulation (engine/src/softbody/WebGpuSoftBodyBackend.cpp drives it): the CPU backend's
// XPBD (engine/src/physics/CpuSoftBodyBackend.cpp: one distance constraint per edge, one volume constraint per
// tetrahedron, compliances from Young's modulus and Poisson's ratio, sub-steps, ground plane with friction) run by ONE
// workgroup per body. The constraints are coloured on the CPU so no two of one colour share a node; the workgroup solves a
// colour in parallel, then waits at a barrier before the next, which is Gauss-Seidel over the colours. A whole step (all
// sub-steps) is one dispatch of `simulate`.

struct Params {
    gravity: vec3<f32>,
    h: f32,                 // the sub-step
    push: vec3<f32>,        // added to every node's velocity at the start of the step (SoftBody AddVelocity)
    keep: f32,              // 1 / (1 + damping * h): the velocity kept per sub-step
    nodeCount: u32,
    edgeCount: u32,
    tetCount: u32,
    subSteps: u32,
    iterations: u32,
    edgeColors: u32,        // ranges[0, edgeColors) are the edge colours, ranges[edgeColors, + tetColors) the tetrahedra's
    tetColors: u32,
    groundEnabled: u32,
    edgeAlphaBase: f32,     // 1 / (shear modulus * h^2), divided by an edge's rest length
    volumeAlphaBase: f32,   // 1 / (bulk modulus * h^2), times a tetrahedron's rest volume
    groundHeight: f32,
    friction: f32,
    invMassScale: f32,      // the density the masses were made with over the density now
    pad0: f32,
    pad1: f32,
    pad2: f32,
}

struct Edge {
    a: u32,
    b: u32,
    rest: f32,
    pad: u32,
}

struct Tet {
    nodes: vec4<u32>,
    rest: f32,              // rest volume; 0 for a degenerate tetrahedron (no constraint)
    pad0: f32,
    pad1: f32,
    pad2: f32,
}

const WORKGROUP: u32 = 256u;
const MIN_LENGTH: f32 = 1e-9;

@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var<storage, read_write> pos: array<vec4<f32>>;   // xyz position, w inverse mass (at the first density)
@group(0) @binding(2) var<storage, read_write> prev: array<vec4<f32>>;  // xyz position at the start of the sub-step
@group(0) @binding(3) var<storage, read_write> vel: array<vec4<f32>>;
@group(0) @binding(4) var<storage, read> edges: array<Edge>;           // sorted by colour
@group(0) @binding(5) var<storage, read> tets: array<Tet>;             // sorted by colour
@group(0) @binding(6) var<storage, read> ranges: array<vec2<u32>>;     // [first, end) of each colour
@group(0) @binding(7) var<storage, read_write> lambdas: array<f32>;     // edges first, then tetrahedra

fn inv_mass(i: u32) -> f32 {
    return pos[i].w * params.invMassScale;
}

fn solve_edge(e: u32) {
    let edge = edges[e];
    let wa = inv_mass(edge.a);
    let wb = inv_mass(edge.b);
    let wSum = wa + wb;
    if (wSum <= 0.0 || edge.rest <= MIN_LENGTH) {
        return;
    }
    let pa = pos[edge.a].xyz;
    let pb = pos[edge.b].xyz;
    let d = pa - pb;
    let len = length(d);
    if (len < MIN_LENGTH) {
        return;
    }
    let alpha = params.edgeAlphaBase / edge.rest;
    let c = len - edge.rest;
    let dl = (-c - alpha * lambdas[e]) / (wSum + alpha);
    lambdas[e] += dl;
    let scale = dl / len;
    pos[edge.a] = vec4<f32>(pa + d * (wa * scale), pos[edge.a].w);
    pos[edge.b] = vec4<f32>(pb - d * (wb * scale), pos[edge.b].w);
}

fn solve_tet(t: u32) {
    let tet = tets[t];
    if (tet.rest <= 0.0) {
        return;
    }
    let n = tet.nodes;
    let p0 = pos[n.x].xyz;
    let p1 = pos[n.y].xyz;
    let p2 = pos[n.z].xyz;
    let p3 = pos[n.w].xyz;
    let e1 = p1 - p0;
    let e2 = p2 - p0;
    let e3 = p3 - p0;
    // The volume's gradients with respect to nodes 1, 2 and 3 (node 0's is minus their sum)
    let g1 = cross(e2, e3) / 6.0;
    let g2 = cross(e3, e1) / 6.0;
    let g3 = cross(e1, e2) / 6.0;
    let g0 = -(g1 + g2 + g3);
    let volume = dot(g1, e1);
    let w0 = inv_mass(n.x);
    let w1 = inv_mass(n.y);
    let w2 = inv_mass(n.z);
    let w3 = inv_mass(n.w);
    let alpha = params.volumeAlphaBase * tet.rest;
    let denominator = w0 * dot(g0, g0) + w1 * dot(g1, g1) + w2 * dot(g2, g2) + w3 * dot(g3, g3) + alpha;
    if (!(denominator > 0.0)) {
        return;
    }
    let slot = params.edgeCount + t;
    let c = volume - tet.rest;
    let dl = (-c - alpha * lambdas[slot]) / denominator;
    lambdas[slot] += dl;
    pos[n.x] = vec4<f32>(p0 + g0 * (w0 * dl), pos[n.x].w);
    pos[n.y] = vec4<f32>(p1 + g1 * (w1 * dl), pos[n.y].w);
    pos[n.z] = vec4<f32>(p2 + g2 * (w2 * dl), pos[n.z].w);
    pos[n.w] = vec4<f32>(p3 + g3 * (w3 * dl), pos[n.w].w);
}

@compute @workgroup_size(256) fn simulate(@builtin(local_invocation_index) lid: u32) {
    let constraintCount = params.edgeCount + params.tetCount;
    for (var sub = 0u; sub < params.subSteps; sub++) {
        // Predict
        for (var i = lid; i < params.nodeCount; i += WORKGROUP) {
            let p = pos[i];
            prev[i] = vec4<f32>(p.xyz, 0.0);
            if (p.w > 0.0) {
                var v = vel[i].xyz;
                if (sub == 0u) {
                    v += params.push;
                }
                v = (v + params.gravity * params.h) * params.keep;
                vel[i] = vec4<f32>(v, 0.0);
                pos[i] = vec4<f32>(p.xyz + v * params.h, p.w);
            }
        }
        for (var k = lid; k < constraintCount; k += WORKGROUP) {
            lambdas[k] = 0.0;
        }
        storageBarrier();

        for (var iteration = 0u; iteration < params.iterations; iteration++) {
            for (var colour = 0u; colour < params.edgeColors; colour++) {
                let bounds = ranges[colour];
                for (var e = bounds.x + lid; e < bounds.y; e += WORKGROUP) {
                    solve_edge(e);
                }
                storageBarrier();
            }
            for (var colour = 0u; colour < params.tetColors; colour++) {
                let bounds = ranges[params.edgeColors + colour];
                for (var t = bounds.x + lid; t < bounds.y; t += WORKGROUP) {
                    solve_tet(t);
                }
                storageBarrier();
            }
        }

        // The ground plane (clamp, and friction * depth off the sideways motion), then the velocities from the motion
        for (var i = lid; i < params.nodeCount; i += WORKGROUP) {
            var p = pos[i];
            let start = prev[i].xyz;
            if (params.groundEnabled == 1u && p.y < params.groundHeight) {
                let depth = params.groundHeight - p.y;
                p.y = params.groundHeight;
                let side = vec2<f32>(p.x - start.x, p.z - start.z);
                let slide = length(side);
                if (slide > MIN_LENGTH) {
                    let kept = max(0.0, slide - params.friction * depth) / slide;
                    p.x = start.x + side.x * kept;
                    p.z = start.z + side.y * kept;
                }
                pos[i] = p;
            }
            vel[i] = vec4<f32>((p.xyz - start) / params.h, 0.0);
        }
        storageBarrier();
    }
}
