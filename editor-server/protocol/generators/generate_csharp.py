#!/usr/bin/env python3
# protocol/generators/generate_csharp.py

import json
from pathlib import Path

SCRIPT_DIR = Path(__file__).parent
PROTOCOL_PATH = Path(__file__).parent.parent / "protocol.json"
OUTPUT_PATH = Path(__file__).parent.parent.parent / "clients" / "csharp" / "Protocol.generated.cs"

TYPE_MAP = {
    "uint8": "byte",
    "uint32": "uint",
    "int32": "int",
    "float32": "float",
    "bool": "bool",
    "string": "string",
    "bytes": "byte[]",
    "vec3": "Vec3",
}

def get_cs_type(ftype: str, protocol: dict) -> str:
    """Convert protocol type to C# type."""
    # Check for array types (e.g., "EntityInfo[]")
    if ftype.endswith("[]"):
        base_type = ftype[:-2]
        inner_type = get_cs_type(base_type, protocol)
        return f"{inner_type}[]"

    # Check built-in types
    if ftype in TYPE_MAP:
        return TYPE_MAP[ftype]

    # Check custom types defined in protocol
    if "types" in protocol and ftype in protocol["types"]:
        return ftype  # Will be generated as a struct

    return ftype

def to_pascal_case(name: str) -> str:
    """Convert field name to PascalCase."""
    return ''.join(word.capitalize() for word in name.split('_'))

def generate():
    with open(PROTOCOL_PATH) as f:
        protocol = json.load(f)

    lines = [
        "// Auto-generated from protocol.json - do not edit",
        "using System;",
        "",
        "namespace N2Engine.Editor.Protocol",
        "{",
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
            lines.append(f"    public struct {type_name}")
            lines.append("    {")
            for field, ftype in type_def.items():
                cs_type = get_cs_type(ftype, protocol)
                lines.append(f"        public {cs_type} {to_pascal_case(field)};")
            lines.append("    }")
            lines.append("")

    # Generate request structs
    for name, cmd in protocol["commands"].items():
        if cmd["request"]:
            lines.append(f"    public struct {name}Request")
            lines.append("    {")
            for field, ftype in cmd["request"].items():
                cs_type = get_cs_type(ftype, protocol)
                lines.append(f"        public {cs_type} {to_pascal_case(field)};")
            lines.append("    }")
            lines.append("")

    # Generate response structs (deduplicated)
    generated = set()
    for name, cmd in protocol["commands"].items():
        resp = cmd["response"]
        resp_type = resp["type"]
        if resp_type not in ["Ok", "Error"] and "fields" in resp and resp_type not in generated:
            generated.add(resp_type)
            lines.append(f"    public struct {resp_type}Response")
            lines.append("    {")
            for field, ftype in resp["fields"].items():
                cs_type = get_cs_type(ftype, protocol)
                lines.append(f"        public {cs_type} {to_pascal_case(field)};")
            lines.append("    }")
            lines.append("")

    lines.append("}")

    OUTPUT_PATH.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT_PATH.write_text("\n".join(lines))
    print(f"Generated {OUTPUT_PATH}")

if __name__ == "__main__":
    generate()