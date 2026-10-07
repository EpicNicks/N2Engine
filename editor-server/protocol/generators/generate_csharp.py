#!/usr/bin/env python3
# protocol/generators/generate_csharp.py
#
# Declarations only (ids, protocol version, request and response structs, JSON shapes), with no encoding code.
# Nothing uses or tests the C# client, so it is unsupported: only the TypeScript client has generated codecs.
#
# Usage: generate_csharp.py [--out <path>] [--protocol <protocol.json>]   (default: editor-server/clients/csharp/Protocol.generated.cs)

import argparse
from pathlib import Path

import sys

# Importing protocol_spec would otherwise leave a __pycache__ folder in the source tree
sys.dont_write_bytecode = True
import protocol_spec  # noqa: E402

SCRIPT_DIR = Path(__file__).parent
PROTOCOL_PATH = protocol_spec.PROTOCOL_PATH
OUTPUT_PATH = Path(__file__).parent.parent.parent / "clients" / "csharp" / "Protocol.generated.cs"

TYPE_MAP = {
    "uint8": "byte",
    "uint32": "uint",
    "int32": "int",
    "float32": "float",
    "bool": "bool",
    "string": "string",
    "bytes": "byte[]",
    # 16 floats, column-major (element col * 4 + row)
    "mat4": "float[]",
    # JSON text, exactly as on the wire
    "json": "string",
}

# The C# type of each JSON shape primitive (jsonTypes fields)
JSON_SHAPE_MAP = {
    "string": "string",
    "number": "double",
    "bool": "bool",
    "json": "System.Text.Json.JsonElement",
}

def get_cs_type(ftype: str, protocol: dict) -> str:
    """Convert protocol type to C# type."""
    field_type = protocol_spec.parse_type(ftype, protocol)
    if field_type.kind == "array":
        return f"{get_cs_type(field_type.base, protocol)}[]"
    if field_type.kind == "struct":
        return to_pascal_case(field_type.base)
    return TYPE_MAP[field_type.base]

def get_cs_shape_type(shape: str) -> str:
    element, depth = protocol_spec.split_json_shape(shape)
    return JSON_SHAPE_MAP.get(element, element) + "[]" * depth

def to_pascal_case(name: str) -> str:
    """Convert a name to PascalCase."""
    return ''.join(word[:1].upper() + word[1:] for word in name.split('_'))

def field_lines(fields: dict, protocol: dict) -> list:
    lines = []
    for field, ftype in fields.items():
        if ftype.startswith("json"):
            shape = ftype[len("json:"):] if ftype.startswith("json:") else "any JSON value"
            lines.append(f"        /// <summary>JSON text: {shape}</summary>")
        lines.append(f"        public {get_cs_type(ftype, protocol)} {to_pascal_case(field)};")
    return lines

def generate(output_path=None, protocol_path=None):
    output_path = Path(output_path) if output_path else OUTPUT_PATH
    protocol = protocol_spec.load(Path(protocol_path) if protocol_path else PROTOCOL_PATH)

    lines = [
        "// Auto-generated from protocol.json by generate_csharp.py - do not edit",
        "// Declarations only, with no encoding code; nothing uses or tests this client (unsupported).",
        "using System;",
        "",
        "namespace N2Engine.Editor.Protocol",
        "{",
        "    public static class ProtocolInfo",
        "    {",
        "        /// <summary>protocol.json's version (major.minor.patch); Hello sends it</summary>",
        f"        public const string ProtocolVersion = \"{protocol['version']}\";",
        "    }",
        "",
    ]

    # Command enum
    lines.append("    public enum CommandType : byte")
    lines.append("    {")
    for name, cmd in protocol["commands"].items():
        lines.append(f"        {name} = {cmd['id']},")
    lines.extend([
        "    }",
        "",
    ])

    # Response enum
    lines.append("    public enum ResponseType : byte")
    lines.append("    {")
    for name, id in protocol["responses"].items():
        lines.append(f"        {name} = {id},")
    lines.extend([
        "    }",
        "",
    ])

    # Generate custom types
    if "types" in protocol:
        for type_name, type_def in protocol["types"].items():
            lines.append(f"    public struct {to_pascal_case(type_name)}")
            lines.append("    {")
            lines.extend(field_lines(type_def, protocol))
            lines.append("    }")
            lines.append("")

    # JSON shapes of json fields; property names are the JSON keys
    for type_name, shape_fields in protocol.get("jsonTypes", {}).items():
        lines.append(f"    public class {type_name}")
        lines.append("    {")
        for key, shape in shape_fields.items():
            name, optional = protocol_spec.json_key(key)
            lines.append(f"        [System.Text.Json.Serialization.JsonPropertyName(\"{name}\")]")
            optional_marker = "?" if optional else ""
            lines.append(f"        public {get_cs_shape_type(shape)}{optional_marker} {to_pascal_case(name)} {{ get; set; }}")
        lines.append("    }")
        lines.append("")

    # Generate request structs
    for name, request in protocol_spec.commands_with_requests(protocol):
        lines.append(f"    public struct {name}Request")
        lines.append("    {")
        lines.extend(field_lines(request, protocol))
        lines.append("    }")
        lines.append("")

    # Generate response structs (deduplicated)
    for resp_type, _, fields in protocol_spec.responses(protocol):
        if fields:
            lines.append(f"    public struct {resp_type}Response")
            lines.append("    {")
            lines.extend(field_lines(fields, protocol))
            lines.append("    }")
            lines.append("")

    lines.append("}")
    lines.append("")

    protocol_spec.write_if_changed(output_path, "\n".join(lines))

def main(argv=None):
    parser = argparse.ArgumentParser(description="Generate the C# protocol declarations from protocol.json")
    parser.add_argument("--out", type=Path, help=f"output file (default: {OUTPUT_PATH})")
    parser.add_argument("--protocol", type=Path, help=f"the spec to read (default: {PROTOCOL_PATH})")
    args = parser.parse_args(argv)
    generate(args.out, args.protocol)

if __name__ == "__main__":
    main()
