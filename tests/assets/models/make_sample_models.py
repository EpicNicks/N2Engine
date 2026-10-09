#!/usr/bin/env python3
"""Writes the sample models the engine tests load: textured_cube.glb and textured_cube.fbx, next to this script.

A stand-in for a Blender export (decision 13 on #3), generated so it needs no Blender and carries no third-party
licence: a UV-mapped cube mesh with two materials (a procedural 2 x 2 checker texture on three faces, a plain lit
blue on the other three), used by three nodes: "Cube" at the origin, its child "Child" (above it, half size) and
"Mirrored" (to the right, scale -1 on x). The checker is red | green over blue | yellow, as an image editor shows
it. Standard library only; run it with any Python 3:

    python tests/assets/models/make_sample_models.py

The output is deterministic, so re-running it gives the same bytes. A real Blender export should be added later
(see README.md).

textured_cube.fbx is the same scene as an ASCII FBX 7.4 file (for the optional ufbx importer, #3 P5), laid out the
way Blender's FBX exporter does: centimetres, z up and -y forward, so a correct import has to convert axes and units
to the engine's y up and metres. Its texture is the file name "checker.png" beside it, which is not committed (the
tests write it): an FBX texture names a file.

    python tests/assets/models/make_sample_models.py       # both files
    python tests/assets/models/make_sample_models.py fbx   # only the FBX (the PNG inside the .glb is compressed by
                                                           # the local zlib, whose bytes may differ between versions)
"""

import json
import os
import struct
import sys
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


def fbx_numbers(values):
    """Numbers as FBX ASCII writes them: shortest round-trip text, comma separated"""
    return ",".join(("%d" % v) if float(v).is_integer() else repr(float(v)) for v in values)


def fbx_array(name, values, indent="\t\t"):
    return "%s%s: *%d {\n%s\ta: %s\n%s}\n" % (indent, name, len(values), indent, fbx_numbers(values), indent)


def write_fbx(path):
    """textured_cube.fbx: the cube as one Mesh geometry (6 quads, normals and UVs by polygon vertex, materials by
    polygon) shared by three models: Cube, its child Child and Mirrored, as in textured_cube.glb. Z up, -y
    forward, centimetres: the engine's (x, y, z) metres is the file's (100x, -100z, 100y) centimetres."""
    vertices = cube_faces()

    positions = []
    normals = []
    uvs = []
    for p, n, uv in vertices:
        positions += [100.0 * p[0], -100.0 * p[2], 100.0 * p[1]]
        normals += [n[0], -n[2], n[1]]
        uvs += [uv[0], 1.0 - uv[1]]  # FBX is v up
    polygons = []
    for face in range(6):
        b = face * 4
        polygons += [b, b + 1, b + 2, -(b + 3) - 1]  # the last index of a polygon is stored as -(i) - 1
    uv_index = [i % 4 for i in range(24)]
    unique_uvs = uvs[:8]  # the first face's four corners

    q = '"'
    out = []
    out.append("; FBX 7.4.0 project file\n; Written by N2Engine make_sample_models.py\n")
    out.append("; ------------------------------------------------------------------\n\n")
    out.append("FBXHeaderExtension:  {\n\tFBXHeaderVersion: 1003\n\tFBXVersion: 7400\n"
               "\tCreator: " + q + "N2Engine make_sample_models.py" + q + "\n}\n")

    def prop(name, kind, label, value, indent="\t\t"):
        return '%sP: "%s", "%s", "%s", "",%s\n' % (indent, name, kind, label, value)

    out.append("GlobalSettings:  {\n\tVersion: 1000\n\tProperties70:  {\n")
    out.append(prop("UpAxis", "int", "Integer", 2))
    out.append(prop("UpAxisSign", "int", "Integer", 1))
    out.append(prop("FrontAxis", "int", "Integer", 1))
    out.append(prop("FrontAxisSign", "int", "Integer", -1))
    out.append(prop("CoordAxis", "int", "Integer", 0))
    out.append(prop("CoordAxisSign", "int", "Integer", 1))
    out.append(prop("UnitScaleFactor", "double", "Number", 1))
    out.append(prop("OriginalUnitScaleFactor", "double", "Number", 1))
    out.append("\t}\n}\n\n")

    out.append("Objects:  {\n")
    out.append('\tGeometry: 1000, "Geometry::Cube", "Mesh" {\n')
    out.append(fbx_array("Vertices", positions))
    out.append(fbx_array("PolygonVertexIndex", polygons))
    out.append("\t\tGeometryVersion: 124\n")
    out.append('\t\tLayerElementNormal: 0 {\n\t\t\tVersion: 101\n\t\t\tName: ""\n'
               '\t\t\tMappingInformationType: "ByPolygonVertex"\n'
               '\t\t\tReferenceInformationType: "Direct"\n')
    out.append(fbx_array("Normals", normals, "\t\t\t"))
    out.append("\t\t}\n")
    out.append('\t\tLayerElementUV: 0 {\n\t\t\tVersion: 101\n\t\t\tName: "UVMap"\n'
               '\t\t\tMappingInformationType: "ByPolygonVertex"\n'
               '\t\t\tReferenceInformationType: "IndexToDirect"\n')
    out.append(fbx_array("UV", unique_uvs, "\t\t\t"))
    out.append(fbx_array("UVIndex", uv_index, "\t\t\t"))
    out.append("\t\t}\n")
    out.append('\t\tLayerElementMaterial: 0 {\n\t\t\tVersion: 101\n\t\t\tName: ""\n'
               '\t\t\tMappingInformationType: "ByPolygon"\n'
               '\t\t\tReferenceInformationType: "IndexToDirect"\n')
    out.append(fbx_array("Materials", [0, 0, 0, 1, 1, 1], "\t\t\t"))
    out.append("\t\t}\n")
    out.append("\t\tLayer: 0 {\n\t\t\tVersion: 100\n")
    for element in ("LayerElementNormal", "LayerElementMaterial", "LayerElementUV"):
        out.append('\t\t\tLayerElement:  {\n\t\t\t\tType: "%s"\n\t\t\t\tTypedIndex: 0\n\t\t\t}\n' % element)
    out.append("\t\t}\n")
    out.append("\t}\n")

    def model(model_id, name, translation, scaling):
        return ('\tModel: %d, "Model::%s", "Mesh" {\n\t\tVersion: 232\n\t\tProperties70:  {\n'
                '\t\t\tP: "Lcl Translation", "Lcl Translation", "", "A",%s\n'
                '\t\t\tP: "Lcl Scaling", "Lcl Scaling", "", "A",%s\n\t\t}\n'
                '\t\tShading: Y\n\t\tCulling: "CullingOff"\n\t}\n'
                % (model_id, name, fbx_numbers(translation), fbx_numbers(scaling)))

    # Z up, centimetres: the glb's Child at (0, 1, 0) m is (0, 0, 100) here, Mirrored at (2, 0, 0) m is (200, 0, 0)
    out.append(model(2001, "Cube", (0, 0, 0), (1, 1, 1)))
    out.append(model(2002, "Child", (0, 0, 100), (0.5, 0.5, 0.5)))
    out.append(model(2003, "Mirrored", (200, 0, 0), (-1, 1, 1)))

    def material(material_id, name, diffuse):
        return ('\tMaterial: %d, "Material::%s", "" {\n\t\tVersion: 102\n\t\tShadingModel: "phong"\n'
                '\t\tMultiLayer: 0\n\t\tProperties70:  {\n'
                '\t\t\tP: "DiffuseColor", "Color", "", "A",%s\n\t\t}\n\t}\n'
                % (material_id, name, fbx_numbers(diffuse)))

    out.append(material(3001, "Checker", (1, 1, 1)))
    out.append(material(3002, "Blue", (0.2, 0.4, 1.0)))
    out.append('\tTexture: 4001, "Texture::Checker", "" {\n\t\tType: "TextureVideoClip"\n\t\tVersion: 202\n'
               '\t\tTextureName: "Texture::Checker"\n\t\tProperties70:  {\n'
               '\t\t\tP: "UseMaterial", "bool", "", "",1\n'
               '\t\t\tP: "WrapModeU", "enum", "", "",1\n'
               '\t\t\tP: "WrapModeV", "enum", "", "",1\n\t\t}\n'
               '\t\tMedia: "Video::Checker"\n\t\tFileName: "checker.png"\n'
               '\t\tRelativeFilename: "checker.png"\n\t}\n')
    out.append("}\n\n")

    out.append("Connections:  {\n")
    connections = [(2001, 0), (2002, 2001), (2003, 0), (1000, 2001), (1000, 2002), (1000, 2003)]
    for model_id in (2001, 2002, 2003):
        connections += [(3001, model_id), (3002, model_id)]
    for child, parent in connections:
        out.append('\tC: "OO",%d,%d\n' % (child, parent))
    out.append('\tC: "OP",4001,3001, "DiffuseColor"\n')
    out.append("}\n")

    with open(path, "wb") as handle:  # binary mode: LF line ends on every platform
        handle.write("".join(out).encode("ascii"))


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    write_fbx(os.path.join(here, "textured_cube.fbx"))
    if len(sys.argv) > 1 and sys.argv[1] == "fbx":
        return
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
