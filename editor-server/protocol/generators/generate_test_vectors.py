#!/usr/bin/env python3
# editor-server/protocol/generators/generate_test_vectors.py
#
# Writes the golden vectors: for every request with fields and every response, deterministic sample field values and
# the payload bytes protocol.json says they encode to, from an encoder written here, independently of the server's
# hand-written C++ codecs and the generated TypeScript ones. The C++ tests decode each request payload with the
# server's deserializer and build each response with the server's builder; the TypeScript tests encode and decode
# every vector with the generated codecs. Agreeing with these bytes keeps the three implementations in step.
#
# Sample values come from each field's type and position, so a new command gets vectors without editing this script.
# Strings are non-ASCII (their byte count differs from their length), int32s negative, floats exact in float32, and
# the uint32 0x12345678 + n shows the byte order.
#
# Usage: generate_test_vectors.py [--out <path>] [--protocol <protocol.json>]   (default: editor-server/protocol/test-vectors.json)

import argparse
import json
import struct
from pathlib import Path

import sys

# Importing protocol_spec would otherwise leave a __pycache__ folder in the source tree
sys.dont_write_bytecode = True
import protocol_spec  # noqa: E402

PROTOCOL_PATH = protocol_spec.PROTOCOL_PATH
OUTPUT_PATH = Path(__file__).parent.parent / "test-vectors.json"

# The array length of every sample array
ARRAY_LENGTH = 2


def json_text(value) -> str:
    """Canonical JSON text: compact, keys sorted (as nlohmann::json writes them), UTF-8 rather than \\u escapes"""
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"), sort_keys=True)


def sample_shape(shape: str, protocol: dict, label: str, index: int):
    element, depth = protocol_spec.split_json_shape(shape)
    if depth > 0:
        inner = element + "[]" * (depth - 1)
        return [sample_shape(inner, protocol, label, index + i) for i in range(ARRAY_LENGTH)]
    if element == "string":
        return f"{label} {index} ✓"
    if element == "number":
        return 2.5 + index
    if element == "bool":
        return index % 2 == 0
    if element == "json":
        return {"list": [index, "two ✓", True, None], "nested": {"value": 1.5}}
    shape_fields = protocol["jsonTypes"][element]
    value = {}
    for offset, (key, inner) in enumerate(shape_fields.items()):
        name, _ = protocol_spec.json_key(key)
        value[name] = sample_shape(inner, protocol, name, index + offset)
    return dict(sorted(value.items()))


def sample(raw: str, protocol: dict, label: str, index: int):
    """A deterministic value for a field of type raw; index varies it by position"""
    field_type = protocol_spec.parse_type(raw, protocol)
    if field_type.kind == "array":
        return [sample(field_type.base, protocol, label, index + i) for i in range(ARRAY_LENGTH)]
    if field_type.kind == "struct":
        fields = protocol["types"][field_type.base]
        return {name: sample(inner, protocol, name, index + offset)
                for offset, (name, inner) in enumerate(fields.items())}
    if field_type.kind == "json":
        return sample_shape(field_type.json_shape or "json", protocol, label, index)
    base = field_type.base
    if base == "uint8":
        return 0x80 + index
    if base == "uint32":
        return 0x12345678 + index
    if base == "int32":
        return -123456 - index
    if base == "float32":
        # Multiples of 0.25 are exact in float32
        return (1.25 + 0.25 * index) * (-1 if index % 2 else 1)
    if base == "bool":
        return index % 2 == 0
    if base == "string":
        return f"{label} {index} ✓ é"
    if base == "bytes":
        return bytes((index + i * 37) % 256 for i in range(8)).hex()
    if base == "mat4":
        return [(i + 1) * 0.5 * (-1 if i % 3 == 0 else 1) for i in range(16)]
    raise ValueError(f"no sample for {raw}")


def sample_fields(fields: dict, protocol: dict) -> dict:
    counts = protocol_spec.array_count_fields(fields, protocol)
    values = {name: sample(raw, protocol, name, index) for index, (name, raw) in enumerate(fields.items())}
    for count, array in counts.items():
        values[count] = len(values[array])
    return values


def encode(raw: str, value, protocol: dict) -> bytes:
    field_type = protocol_spec.parse_type(raw, protocol)
    if field_type.kind == "array":
        return b"".join(encode(field_type.base, element, protocol) for element in value)
    if field_type.kind == "struct":
        return b"".join(encode(inner, value[name], protocol) for name, inner in protocol["types"][field_type.base].items())
    if field_type.kind == "json":
        return encode("string", json_text(value), protocol)
    base = field_type.base
    if base == "uint8":
        return struct.pack("<B", value)
    if base == "uint32":
        return struct.pack("<I", value)
    if base == "int32":
        return struct.pack("<i", value)
    if base == "float32":
        return struct.pack("<f", value)
    if base == "bool":
        return struct.pack("<B", 1 if value else 0)
    if base == "string":
        data = value.encode("utf-8")
        return struct.pack("<I", len(data)) + data
    if base == "bytes":
        return bytes.fromhex(value)
    if base == "mat4":
        return struct.pack("<16f", *value)
    raise ValueError(f"can't encode {raw}")


def encode_fields(fields: dict, values: dict, protocol: dict) -> bytes:
    return b"".join(encode(raw, values[name], protocol) for name, raw in fields.items())


def build(protocol: dict) -> dict:
    requests = []
    for name, request in protocol_spec.commands_with_requests(protocol):
        values = sample_fields(request, protocol)
        requests.append({
            "command": name,
            "id": protocol["commands"][name]["id"],
            "fields": values,
            "payload": encode_fields(request, values, protocol).hex(),
        })

    responses = []
    for name, response_id, fields in protocol_spec.responses(protocol):
        if name == "Ok":
            values, payload = {}, b""
        elif name == "Error":
            # The documented special case: the message's raw UTF-8 bytes, no length prefix
            values = {"message": "Something failed ✓ é"}
            payload = values["message"].encode("utf-8")
        else:
            values = sample_fields(fields, protocol)
            payload = encode_fields(fields, values, protocol)
        responses.append({
            "response": name,
            "id": protocol["responses"][name],
            "fields": values,
            "payload": payload.hex(),
        })

    return {
        "description": "Golden vectors, generated from protocol.json by generators/generate_test_vectors.py - do not edit. "
                       "payload is the hex of the payload (no frame header) that fields encode to. "
                       "bytes fields are hex strings, mat4 16 numbers (column-major), json fields their parsed value "
                       "(encoded as compact JSON with sorted keys); Error's payload is its message's raw UTF-8 bytes.",
        "protocolVersion": protocol["version"],
        "requests": requests,
        "responses": responses,
    }


def generate(output_path=None, protocol_path=None):
    output_path = Path(output_path) if output_path else OUTPUT_PATH
    protocol = protocol_spec.load(Path(protocol_path) if protocol_path else PROTOCOL_PATH)
    content = json.dumps(build(protocol), indent=2, ensure_ascii=False) + "\n"
    protocol_spec.write_if_changed(output_path, content)


def main(argv=None):
    parser = argparse.ArgumentParser(description="Generate the protocol's golden vectors from protocol.json")
    parser.add_argument("--out", type=Path, help=f"output file (default: {OUTPUT_PATH})")
    parser.add_argument("--protocol", type=Path, help=f"the spec to read (default: {PROTOCOL_PATH})")
    args = parser.parse_args(argv)
    generate(args.out, args.protocol)


if __name__ == "__main__":
    main()
