// The WebGPU particle backend's simulation (engine/src/particles/WebGpuParticleBackend.cpp drives it): position-based
// particles on a hashed uniform grid, one system per set of buffers. Fluids (Water, Smoke) solve a density constraint
// (position based fluids, Macklin and Muller 2013, with the constraint one-sided so a free surface does not clump);
// granular material (Sand) solves contact constraints with friction. Every particle also keeps above the optional
// ground plane. A step on the CPU side is, per sub-step:
//   clear_cells, predict, then `iterations` times (lambdas [fluid only], deltas, apply), then finalize.
// Slots: pos.w is the particle's remaining lifetime in seconds; a slot with pos.w <= 0 is empty and is skipped. The CPU
// owns which slots are alive (it writes a spawned particle's slot and counts its lifetime down the same way).

struct Params {
    gravity: vec3<f32>,
    dt: f32,                // the sub-step
    count: u32,             // slots [0, count) are looked at
    hashMask: u32,          // the cell table has hashMask + 1 buckets (a power of two)
    cellCapacity: u32,      // particles one bucket keeps (more are left out of the neighbour search)
    granular: u32,          // 1: contact constraints with friction (Sand); 0: the density constraint (Water, Smoke)
    radius: f32,            // the kernel radius h (also the grid's cell size)
    invCell: f32,           // 1 / radius
    restDensity: f32,       // the density of particles at rest spacing, in kernel units
    spacing: f32,           // the rest distance between particles
    keep: f32,              // the share of its velocity a particle keeps per sub-step (drag and the preset's damping)
    friction: f32,          // granular contact and ground friction, 0..1
    tensile: f32,           // the fluid's artificial pressure (surface tension), 0 for none
    epsilon: f32,           // the density constraint's relaxation
    groundEnabled: u32,
    groundHeight: f32,
    bounce: f32,            // the share of its speed a particle keeps off the ground
    poly6: f32,             // 315 / (64 pi h^9)
    spikyGrad: f32,         // -45 / (pi h^6)
    invTensileW: f32,       // 1 / W(0.2 h): the artificial pressure's reference
    maxMove: f32,           // the most a particle moves in one solver iteration
    lifetimeStep: f32,      // the lifetime a particle loses this sub-step (the sub-step, or 0 to keep it)
}

@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var<storage, read_write> pos: array<vec4<f32>>;       // xyz position, w remaining lifetime
@group(0) @binding(2) var<storage, read_write> vel: array<vec4<f32>>;       // xyz velocity, w 1 when it touched the ground
@group(0) @binding(3) var<storage, read_write> predicted: array<vec4<f32>>; // xyz predicted position, w lambda
@group(0) @binding(4) var<storage, read_write> delta: array<vec4<f32>>;     // xyz correction of this iteration
@group(0) @binding(5) var<storage, read_write> cellCount: array<atomic<u32>>;
@group(0) @binding(6) var<storage, read_write> cellItems: array<u32>;

fn cell_of(p: vec3<f32>) -> vec3<i32> {
    return vec3<i32>(floor(p * params.invCell));
}

fn bucket_of(c: vec3<i32>) -> u32 {
    let u = bitcast<vec3<u32>>(c);
    return ((u.x * 73856093u) ^ (u.y * 19349663u) ^ (u.z * 83492791u)) & params.hashMask;
}

fn alive(i: u32) -> bool {
    return pos[i].w > 0.0;
}

fn poly6(r2: f32) -> f32 {
    let h2 = params.radius * params.radius;
    if (r2 >= h2) {
        return 0.0;
    }
    let d = h2 - r2;
    return params.poly6 * d * d * d;
}

// The spiky kernel's gradient for the offset d (from j to i) of length r
fn spiky_grad(d: vec3<f32>, r: f32) -> vec3<f32> {
    if (r <= 1e-6 || r >= params.radius) {
        return vec3<f32>(0.0);
    }
    let x = params.radius - r;
    return d * (params.spikyGrad * x * x / r);
}

@compute @workgroup_size(64) fn clear_cells(@builtin(global_invocation_id) gid: vec3<u32>) {
    let i = gid.x;
    if (i <= params.hashMask) {
        atomicStore(&cellCount[i], 0u);
    }
}

@compute @workgroup_size(64) fn predict(@builtin(global_invocation_id) gid: vec3<u32>) {
    let i = gid.x;
    if (i >= params.count) {
        return;
    }
    let p = pos[i];
    if (p.w <= 0.0) {
        predicted[i] = vec4<f32>(p.xyz, 0.0);
        return;
    }
    let v = (vel[i].xyz + params.gravity * params.dt) * params.keep;
    vel[i] = vec4<f32>(v, 0.0);
    let q = p.xyz + v * params.dt;
    predicted[i] = vec4<f32>(q, 0.0);
    let b = bucket_of(cell_of(q));
    let slot = atomicAdd(&cellCount[b], 1u);
    if (slot < params.cellCapacity) {
        cellItems[b * params.cellCapacity + slot] = i;
    }
}

@compute @workgroup_size(64) fn lambdas(@builtin(global_invocation_id) gid: vec3<u32>) {
    let i = gid.x;
    if (i >= params.count || !alive(i)) {
        return;
    }
    let pi = predicted[i].xyz;
    let home = cell_of(pi);
    var density = poly6(0.0);
    var gradI = vec3<f32>(0.0);
    var sumSq = 0.0;
    for (var dz = -1; dz <= 1; dz++) {
        for (var dy = -1; dy <= 1; dy++) {
            for (var dx = -1; dx <= 1; dx++) {
                let b = bucket_of(home + vec3<i32>(dx, dy, dz));
                let n = min(atomicLoad(&cellCount[b]), params.cellCapacity);
                for (var k = 0u; k < n; k++) {
                    let j = cellItems[b * params.cellCapacity + k];
                    if (j == i) {
                        continue;
                    }
                    let d = pi - predicted[j].xyz;
                    let r2 = dot(d, d);
                    if (r2 >= params.radius * params.radius) {
                        continue;
                    }
                    density += poly6(r2);
                    let g = spiky_grad(d, sqrt(r2)) / params.restDensity;
                    gradI += g;
                    sumSq += dot(g, g);
                }
            }
        }
    }
    // One-sided: only a particle packed denser than at rest is pushed apart, so the free surface does not pull together
    let c = max(density / params.restDensity - 1.0, 0.0);
    sumSq += dot(gradI, gradI);
    predicted[i].w = -c / (sumSq + params.epsilon);
}

@compute @workgroup_size(64) fn deltas(@builtin(global_invocation_id) gid: vec3<u32>) {
    let i = gid.x;
    if (i >= params.count || !alive(i)) {
        return;
    }
    let me = predicted[i];
    let pi = me.xyz;
    let home = cell_of(pi);
    var shift = vec3<f32>(0.0);
    var contacts = 0.0;
    for (var dz = -1; dz <= 1; dz++) {
        for (var dy = -1; dy <= 1; dy++) {
            for (var dx = -1; dx <= 1; dx++) {
                let b = bucket_of(home + vec3<i32>(dx, dy, dz));
                let n = min(atomicLoad(&cellCount[b]), params.cellCapacity);
                for (var k = 0u; k < n; k++) {
                    let j = cellItems[b * params.cellCapacity + k];
                    if (j == i) {
                        continue;
                    }
                    let other = predicted[j];
                    let d = pi - other.xyz;
                    let r2 = dot(d, d);
                    if (params.granular == 1u) {
                        if (r2 >= params.spacing * params.spacing || r2 <= 1e-12) {
                            continue;
                        }
                        let r = sqrt(r2);
                        let normal = d / r;
                        // Half the overlap each, then friction: the relative sideways motion of this sub-step is cut
                        // by friction (Coulomb in PBD: at most friction * the normal correction)
                        let overlap = params.spacing - r;
                        var correction = normal * (0.5 * overlap);
                        let relative = (pi - pos[i].xyz) - (other.xyz - pos[j].xyz);
                        let tangential = relative - normal * dot(relative, normal);
                        let slide = length(tangential);
                        if (slide > 1e-9) {
                            // Static below friction * overlap (all the sideways motion undone), kinetic above it
                            correction -= tangential * (min(slide, params.friction * overlap) / slide) * 0.5;
                        }
                        shift += correction;
                        contacts += 1.0;
                    } else {
                        if (r2 >= params.radius * params.radius) {
                            continue;
                        }
                        var tensile = 0.0;
                        if (params.tensile > 0.0) {
                            let ratio = poly6(r2) * params.invTensileW;
                            tensile = -params.tensile * ratio * ratio * ratio * ratio;
                        }
                        shift += (me.w + other.w + tensile) * spiky_grad(d, sqrt(r2)) / params.restDensity;
                    }
                }
            }
        }
    }
    if (params.granular == 1u && contacts > 1.0) {
        shift /= contacts; // averaged, so a particle in a pile is not pushed out by the sum of all its contacts
    }
    let length2 = dot(shift, shift);
    if (length2 > params.maxMove * params.maxMove) {
        shift *= params.maxMove / sqrt(length2);
    }
    delta[i] = vec4<f32>(shift, contacts); // w: the grains it touched (granular), read by finalize
}

@compute @workgroup_size(64) fn apply(@builtin(global_invocation_id) gid: vec3<u32>) {
    let i = gid.x;
    if (i >= params.count || !alive(i)) {
        return;
    }
    var q = predicted[i].xyz + delta[i].xyz;
    if (params.groundEnabled == 1u && q.y < params.groundHeight) {
        // On the plane, and friction * depth off the sideways motion of this sub-step
        let depth = params.groundHeight - q.y;
        q.y = params.groundHeight;
        let start = pos[i].xyz;
        let side = vec2<f32>(q.x - start.x, q.z - start.z);
        let slide = length(side);
        if (slide > 1e-9) {
            let kept = max(0.0, slide - params.friction * depth) / slide;
            q.x = start.x + side.x * kept;
            q.z = start.z + side.y * kept;
        }
        vel[i].w = 1.0;
    }
    predicted[i] = vec4<f32>(q, predicted[i].w);
}

@compute @workgroup_size(64) fn finalize(@builtin(global_invocation_id) gid: vec3<u32>) {
    let i = gid.x;
    if (i >= params.count) {
        return;
    }
    let p = pos[i];
    if (p.w <= 0.0) {
        return;
    }
    let q = predicted[i].xyz;
    let before = vel[i];
    var v = (q - p.xyz) / params.dt;
    if (before.w > 0.5 && before.y < 0.0) {
        // Off the ground with `bounce` of the speed it came down with
        v.y = max(v.y, -before.y * params.bounce);
    }
    if (params.granular == 1u && (before.w > 0.5 || delta[i].w > 0.0)) {
        // A grain resting on others or on the ground loses `friction` of its sideways speed each sub-step, so grains
        // stay where they land and pile up instead of flowing out into one layer
        v.x *= 1.0 - params.friction;
        v.z *= 1.0 - params.friction;
    }
    pos[i] = vec4<f32>(q, p.w - params.lifetimeStep);
    vel[i] = vec4<f32>(v, 0.0);
}
