#!/usr/bin/env python3
"""Writes the binary assets of the GPU smoke-test scene (lua_project/assets/scene.lua), next to it.

Everything is this repository's own work, drawn procedurally here, so no third-party asset or licence is involved:

    assets/textures/checker.png         128 x 128: red | green over blue | yellow quadrants in a white border
    assets/textures/alpha_gradient.png  128 x 128: white and orange diagonal stripes, alpha 0 at the left edge
                                        rising to 255 at the right (for the opaque / blend / mask materials)
    assets/ui/icon.png                  64 x 64: blue, a red square in the top-left corner, a white arrow up
    assets/models/smoke_robot.gltf      a small robot: glTF 2.0 JSON with its buffer and its texture embedded as
                                        base64 data: URIs (see ROBOT below)

Standard library only; run it with any Python 3 from anywhere:

    python lua_project/tools/make_smoke_assets.py

The output is deterministic, so re-running it gives the same bytes. The .mat files beside the textures are written
by hand, not by this script.
"""

import base64
import json
import math
import os
import struct
import zlib

ASSETS = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "assets")


# ===== PNG =====

def png_rgba(width, height, pixel):
    """An 8-bit RGBA PNG; pixel(x, y) gives (r, g, b, a) with y = 0 the top row"""

    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    raw = bytearray()
    for y in range(height):
        raw.append(0)  # filter: none
        for x in range(width):
            raw.extend(pixel(x, y))
    header = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(bytes(raw), 9)) +
            chunk(b"IEND", b""))


RED = (230, 30, 30, 255)
GREEN = (30, 200, 60, 255)
BLUE = (40, 80, 230, 255)
YELLOW = (250, 220, 30, 255)
WHITE = (255, 255, 255, 255)


def checker(x, y, size=128, border=6):
    if x < border or y < border or x >= size - border or y >= size - border:
        return WHITE
    top, left = y < size // 2, x < size // 2
    if top:
        return RED if left else GREEN
    return BLUE if left else YELLOW


def alpha_gradient(x, y, size=128):
    stripe = ((x + y) // 12) % 2 == 0
    r, g, b = (255, 255, 255) if stripe else (255, 140, 20)
    alpha = round(255 * x / (size - 1))
    return (r, g, b, alpha)


def icon(x, y, size=64):
    if x < 16 and y < 16:
        return RED
    # A white arrow pointing up: a triangle head over a stem, centred
    cx = size / 2 - 0.5
    if 14 <= y < 34 and abs(x - cx) <= (y - 14) * 0.9:
        return WHITE
    if 34 <= y < 54 and abs(x - cx) <= 5:
        return WHITE
    return (40, 90, 200, 255)


def robot_face(x, y, size=64):
    if x < 3 or y < 3 or x >= size - 3 or y >= size - 3:
        return (60, 60, 70, 255)
    if 16 <= y < 28 and (14 <= x < 26 or 38 <= x < 50):
        return (20, 220, 240, 255)  # eyes
    if 42 <= y < 48 and 16 <= x < 48:
        return (220, 30, 30, 255)  # mouth
    return (200, 200, 210, 255)


# ===== glTF =====

class Gltf:
    """A glTF 2.0 document with one buffer, written as JSON with the buffer and images as base64 data: URIs"""

    def __init__(self):
        self.bin = bytearray()
        self.doc = {"asset": {"version": "2.0", "generator": "N2Engine make_smoke_assets.py"},
                    "bufferViews": [], "accessors": []}

    def view(self, data, target=None):
        while len(self.bin) % 4:
            self.bin.append(0)
        view = {"buffer": 0, "byteOffset": len(self.bin), "byteLength": len(data)}
        if target:
            view["target"] = target
        self.bin.extend(data)
        self.doc["bufferViews"].append(view)
        return len(self.doc["bufferViews"]) - 1

    def accessor(self, view, component_type, count, kind, minimum=None, maximum=None):
        accessor = {"bufferView": view, "componentType": component_type, "count": count, "type": kind}
        if minimum is not None:
            accessor["min"] = minimum
            accessor["max"] = maximum
        self.doc["accessors"].append(accessor)
        return len(self.doc["accessors"]) - 1

    def mesh_primitive(self, vertices, indices, material):
        """A primitive from (position, normal, uv) vertices and triangle indices"""
        positions = b"".join(struct.pack("<3f", *p) for p, _, _ in vertices)
        normals = b"".join(struct.pack("<3f", *n) for _, n, _ in vertices)
        uvs = b"".join(struct.pack("<2f", *uv) for _, _, uv in vertices)
        lo = [min(p[i] for p, _, _ in vertices) for i in range(3)]
        hi = [max(p[i] for p, _, _ in vertices) for i in range(3)]
        count = len(vertices)
        position = self.accessor(self.view(positions, 34962), 5126, count, "VEC3", lo, hi)
        normal = self.accessor(self.view(normals, 34962), 5126, count, "VEC3")
        uv = self.accessor(self.view(uvs, 34962), 5126, count, "VEC2")
        index = self.accessor(self.view(struct.pack("<%dH" % len(indices), *indices), 34963), 5123, len(indices),
                              "SCALAR")
        return {"attributes": {"POSITION": position, "NORMAL": normal, "TEXCOORD_0": uv}, "indices": index,
                "material": material}

    def write(self, path):
        while len(self.bin) % 4:
            self.bin.append(0)
        self.doc["buffers"] = [{"byteLength": len(self.bin),
                                "uri": "data:application/octet-stream;base64," +
                                       base64.b64encode(bytes(self.bin)).decode("ascii")}]
        with open(path, "w", newline="\n") as out:
            json.dump(self.doc, out, indent=2, sort_keys=True)
            out.write("\n")


def sub(a, b):
    return tuple(x - y for x, y in zip(a, b))


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def check_outward(triangle, normal, centre):
    """Every triangle must wind counter-clockwise seen from outside, and its normal point away from the centre"""
    a, b, c = triangle
    winding = cross(sub(b, a), sub(c, a))
    face_centre = tuple(sum(v[i] for v in triangle) / 3 for i in range(3))
    assert dot(winding, normal) > 0, "clockwise triangle"
    assert dot(normal, sub(face_centre, centre)) > 0, "inward normal"


def box(hx, hy, hz):
    """24 vertices (4 per face) and 36 indices; each face maps the whole texture upright (glTF v = 0 at the top)"""
    faces = [
        ((0, 0, 1), [(-hx, -hy, hz), (hx, -hy, hz), (hx, hy, hz), (-hx, hy, hz)]),        # +Z
        ((1, 0, 0), [(hx, -hy, hz), (hx, -hy, -hz), (hx, hy, -hz), (hx, hy, hz)]),        # +X
        ((0, 1, 0), [(-hx, hy, hz), (hx, hy, hz), (hx, hy, -hz), (-hx, hy, -hz)]),        # +Y
        ((0, 0, -1), [(hx, -hy, -hz), (-hx, -hy, -hz), (-hx, hy, -hz), (hx, hy, -hz)]),   # -Z
        ((-1, 0, 0), [(-hx, -hy, -hz), (-hx, -hy, hz), (-hx, hy, hz), (-hx, hy, -hz)]),   # -X
        ((0, -1, 0), [(-hx, -hy, -hz), (hx, -hy, -hz), (hx, -hy, hz), (-hx, -hy, hz)]),   # -Y
    ]
    uvs = [(0.0, 1.0), (1.0, 1.0), (1.0, 0.0), (0.0, 0.0)]  # bottom-left, bottom-right, top-right, top-left
    vertices, indices = [], []
    for normal, corners in faces:
        base = len(vertices)
        for corner, uv in zip(corners, uvs):
            vertices.append((corner, normal, uv))
        indices += [base, base + 1, base + 2, base, base + 2, base + 3]
        check_outward(corners[:3], normal, (0, 0, 0))
        check_outward([corners[0], corners[2], corners[3]], normal, (0, 0, 0))
    return vertices, indices


def pyramid(half, height):
    """A square pyramid standing on y = 0 with its apex at y = height: four flat sides and a base"""
    p = [(-half, 0, half), (half, 0, half), (half, 0, -half), (-half, 0, -half)]
    apex = (0, height, 0)
    centre = (0, height / 4, 0)
    vertices, indices = [], []

    def face(corners, uvs):
        normal = cross(sub(corners[1], corners[0]), sub(corners[2], corners[0]))
        length = math.sqrt(dot(normal, normal))
        normal = tuple(round(n / length, 6) for n in normal)
        check_outward(corners[:3], normal, centre)
        base = len(vertices)
        for corner, uv in zip(corners, uvs):
            vertices.append((corner, normal, uv))
        for i in range(1, len(corners) - 1):
            indices.extend([base, base + i, base + i + 1])

    side_uvs = [(0.0, 1.0), (1.0, 1.0), (0.5, 0.0)]
    face([p[0], p[1], apex], side_uvs)  # front (+Z)
    face([p[1], p[2], apex], side_uvs)  # right (+X)
    face([p[2], p[3], apex], side_uvs)  # back (-Z)
    face([p[3], p[0], apex], side_uvs)  # left (-X)
    face([p[3], p[2], p[1], p[0]], [(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)])  # base (-Y)
    return vertices, indices


def z_rotation(degrees):
    """A glTF rotation quaternion [x, y, z, w] about +Z"""
    half = math.radians(degrees) / 2
    return [0.0, 0.0, round(math.sin(half), 6), round(math.cos(half), 6)]


# ROBOT: three meshes, three materials (one textured), one embedded image, and a four-level hierarchy:
#   Robot (no mesh)
#   └─ Body       mesh Body (1.2 x 1.4 x 0.8 box, material Face: the robot_face texture on every side), at y = 0.7
#      ├─ Arm.L   mesh Limb (0.3 x 1.0 x 0.3 box, material Orange), at x = -0.85, upright
#      ├─ Arm.R   mesh Limb, at x = +0.85, rotated -30 degrees about Z (its top leans outwards, to +X)
#      └─ Head    mesh Head (0.9 x 0.9 base, 0.7 tall pyramid, material Teal), standing on the body's top
#         └─ Antenna  mesh Limb at half size, standing on the head's apex
def make_robot():
    gltf = Gltf()
    gltf.doc["images"] = [{"name": "RobotFace",
                           "uri": "data:image/png;base64," +
                                  base64.b64encode(png_rgba(64, 64, robot_face)).decode("ascii")}]
    gltf.doc["samplers"] = [{"magFilter": 9729, "minFilter": 9729, "wrapS": 33071, "wrapT": 33071}]
    gltf.doc["textures"] = [{"source": 0, "sampler": 0}]
    gltf.doc["materials"] = [
        {"name": "Face", "pbrMetallicRoughness": {"baseColorTexture": {"index": 0}, "metallicFactor": 0.0,
                                                   "roughnessFactor": 0.6}},
        {"name": "Orange", "pbrMetallicRoughness": {"baseColorFactor": [1.0, 0.55, 0.1, 1.0], "metallicFactor": 0.0,
                                                     "roughnessFactor": 0.4}},
        {"name": "Teal", "pbrMetallicRoughness": {"baseColorFactor": [0.1, 0.75, 0.7, 1.0], "metallicFactor": 0.0,
                                                   "roughnessFactor": 0.3}},
    ]
    body_vertices, body_indices = box(0.6, 0.7, 0.4)
    limb_vertices, limb_indices = box(0.15, 0.5, 0.15)
    head_vertices, head_indices = pyramid(0.45, 0.7)
    gltf.doc["meshes"] = [
        {"name": "Body", "primitives": [gltf.mesh_primitive(body_vertices, body_indices, 0)]},
        {"name": "Limb", "primitives": [gltf.mesh_primitive(limb_vertices, limb_indices, 1)]},
        {"name": "Head", "primitives": [gltf.mesh_primitive(head_vertices, head_indices, 2)]},
    ]
    gltf.doc["nodes"] = [
        {"name": "Robot", "children": [1]},
        {"name": "Body", "mesh": 0, "translation": [0.0, 0.7, 0.0], "children": [2, 3, 4]},
        {"name": "Arm.L", "mesh": 1, "translation": [-0.85, 0.0, 0.0]},
        {"name": "Arm.R", "mesh": 1, "translation": [0.85, 0.0, 0.0], "rotation": z_rotation(-30.0)},
        {"name": "Head", "mesh": 2, "translation": [0.0, 0.7, 0.0], "children": [5]},
        {"name": "Antenna", "mesh": 1, "translation": [0.0, 0.95, 0.0], "scale": [0.5, 0.5, 0.5]},
    ]
    gltf.doc["scenes"] = [{"name": "Scene", "nodes": [0]}]
    gltf.doc["scene"] = 0
    return gltf


def write_binary(relative, data):
    path = os.path.join(ASSETS, relative)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as out:
        out.write(data)


def main():
    write_binary("textures/checker.png", png_rgba(128, 128, checker))
    write_binary("textures/alpha_gradient.png", png_rgba(128, 128, alpha_gradient))
    write_binary("ui/icon.png", png_rgba(64, 64, icon))
    os.makedirs(os.path.join(ASSETS, "models"), exist_ok=True)
    make_robot().write(os.path.join(ASSETS, "models", "smoke_robot.gltf"))


if __name__ == "__main__":
    main()
