#!/usr/bin/env python3
"""Writes the sample model the engine tests load: textured_cube.glb, next to this script.

A stand-in for a Blender export (decision 13 on #3), generated so it needs no Blender and carries no third-party
licence: a UV-mapped cube mesh with two materials (a procedural 2 x 2 checker texture on three faces, a plain lit
blue on the other three), used by three nodes: "Cube" at the origin, its child "Child" (above it, half size) and
"Mirrored" (to the right, scale -1 on x). The checker is red | green over blue | yellow, as an image editor shows
it. Standard library only; run it with any Python 3:

    python tests/assets/models/make_sample_models.py

The output is deterministic, so re-running it gives the same bytes. A real Blender export should be added later
(see README.md).
"""

import json
import os
import struct
import zlib


def png_rgba(width, height, rows_top_first):
    """An 8-bit RGBA PNG from rows of (r, g, b, a) tuples, top row first"""

    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    raw = b"".join(b"\x00" + bytes(c for pixel in row for c in pixel) for row in rows_top_first)
    header = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(raw, 9)) +
            chunk(b"IEND", b""))


def checker_png(size=16):
    red, green, blue, yellow = (255, 0, 0, 255), (0, 255, 0, 255), (0, 0, 255, 255), (255, 255, 0, 255)
    half = size // 2
    rows = []
    for y in range(size):
        row = []
        for x in range(size):
            if y < half:
                row.append(red if x < half else green)
            else:
                row.append(blue if x < half else yellow)
        rows.append(row)
    return png_rgba(size, size, rows)


def cube_faces():
    """24 vertices (4 per face, counter-clockwise from outside) as (position, normal, uv), uv with v down (glTF)"""
    h = 0.5
    # Each face: normal, then its corners bottom-left, bottom-right, top-right, top-left seen from outside
    faces = [
        ((0, 0, 1), [(-h, -h, h), (h, -h, h), (h, h, h), (-h, h, h)]),      # +Z
        ((1, 0, 0), [(h, -h, h), (h, -h, -h), (h, h, -h), (h, h, h)]),      # +X
        ((0, 1, 0), [(-h, h, h), (h, h, h), (h, h, -h), (-h, h, -h)]),      # +Y
        ((0, 0, -1), [(h, -h, -h), (-h, -h, -h), (-h, h, -h), (h, h, -h)]),  # -Z
        ((-1, 0, 0), [(-h, -h, -h), (-h, -h, h), (-h, h, h), (-h, h, -h)]),  # -X
        ((0, -1, 0), [(-h, -h, -h), (h, -h, -h), (h, -h, h), (-h, -h, h)]),  # -Y
    ]
    uvs = [(0.0, 1.0), (1.0, 1.0), (1.0, 0.0), (0.0, 0.0)]  # glTF: v = 0 is the image's top
    vertices = []
    for normal, corners in faces:
        for corner, uv in zip(corners, uvs):
            vertices.append((corner, normal, uv))
    return vertices


class Glb:
    def __init__(self):
        self.bin = bytearray()
        self.doc = {"asset": {"version": "2.0", "generator": "N2Engine make_sample_models.py"},
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

    def write(self, path):
        self.doc["buffers"] = [{"byteLength": len(self.bin)}]
        text = json.dumps(self.doc, separators=(",", ":"), sort_keys=True).encode("utf-8")
        text += b" " * (-len(text) % 4)
        binary = bytes(self.bin) + b"\x00" * (-len(self.bin) % 4)
        total = 12 + 8 + len(text) + 8 + len(binary)
        with open(path, "wb") as out:
            out.write(struct.pack("<III", 0x46546C67, 2, total))
            out.write(struct.pack("<II", len(text), 0x4E4F534A) + text)
            out.write(struct.pack("<II", len(binary), 0x004E4942) + binary)


def main():
    glb = Glb()
    vertices = cube_faces()
    positions = b"".join(struct.pack("<3f", *p) for p, _, _ in vertices)
    normals = b"".join(struct.pack("<3f", *n) for _, n, _ in vertices)
    uvs = b"".join(struct.pack("<2f", *uv) for _, _, uv in vertices)
    position = glb.accessor(glb.view(positions, 34962), 5126, 24, "VEC3", [-0.5] * 3, [0.5] * 3)
    normal = glb.accessor(glb.view(normals, 34962), 5126, 24, "VEC3")
    uv = glb.accessor(glb.view(uvs, 34962), 5126, 24, "VEC2")

    def face_indices(first_face, count):
        indices = []
        for face in range(first_face, first_face + count):
            b = face * 4
            indices += [b, b + 1, b + 2, b, b + 2, b + 3]
        return glb.accessor(glb.view(struct.pack("<%dH" % len(indices), *indices), 34963), 5123, len(indices),
                            "SCALAR")

    textured_faces = face_indices(0, 3)  # +Z, +X, +Y
    plain_faces = face_indices(3, 3)     # -Z, -X, -Y

    image_view = glb.view(checker_png())
    glb.doc["images"] = [{"name": "Checker", "bufferView": image_view, "mimeType": "image/png"}]
    glb.doc["samplers"] = [{"magFilter": 9728, "minFilter": 9728, "wrapS": 33071, "wrapT": 33071}]
    glb.doc["textures"] = [{"source": 0, "sampler": 0}]
    glb.doc["materials"] = [
        {"name": "Checker", "pbrMetallicRoughness": {"baseColorTexture": {"index": 0}, "metallicFactor": 0.0,
                                                      "roughnessFactor": 0.5}},
        {"name": "Blue", "pbrMetallicRoughness": {"baseColorFactor": [0.2, 0.4, 1.0, 1.0], "metallicFactor": 0.0,
                                                   "roughnessFactor": 0.25}},
    ]
    attributes = {"POSITION": position, "NORMAL": normal, "TEXCOORD_0": uv}
    glb.doc["meshes"] = [{"name": "Cube", "primitives": [
        {"attributes": attributes, "indices": textured_faces, "material": 0},
        {"attributes": attributes, "indices": plain_faces, "material": 1},
    ]}]
    glb.doc["nodes"] = [
        {"name": "Cube", "mesh": 0, "children": [1]},
        {"name": "Child", "mesh": 0, "translation": [0.0, 1.0, 0.0], "scale": [0.5, 0.5, 0.5]},
        {"name": "Mirrored", "mesh": 0, "translation": [2.0, 0.0, 0.0], "scale": [-1.0, 1.0, 1.0]},
    ]
    glb.doc["scenes"] = [{"name": "Scene", "nodes": [0, 2]}]
    glb.doc["scene"] = 0

    glb.write(os.path.join(os.path.dirname(os.path.abspath(__file__)), "textured_cube.glb"))


if __name__ == "__main__":
    main()
