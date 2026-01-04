#!/usr/bin/env python3
# editor-server/protocol/generators/generate_typescript.py

import json
from pathlib import Path

SCRIPT_DIR = Path(__file__).parent
PROTOCOL_PATH = Path(__file__).parent.parent / "protocol.json"
OUTPUT_PATH = Path(__file__).parent.parent.parent / "clients" / "web" / "protocol.generated.ts"

TYPE_MAP = {
    "uint8": "number",
    "uint32": "number",
    "int32": "number",
    "float32": "number",
    "bool": "boolean",
    "string": "string",
    "bytes": "Uint8Array",
    "vec3": "Vec3",
}

def to_pascal_case(name: str) -> str:
    """Convert name to PascalCase."""
    if name == "vec3":
        return "Vec3"
    # Already PascalCase or camelCase - just ensure first letter is capital
    if not '_' in name and not ' ' in name:
        return name[0].upper() + name[1:] if name else name
    return ''.join(word.capitalize() for word in name.replace('_', ' ').split())

def get_ts_type(ftype: str, protocol: dict) -> str:
    """Convert protocol type to TypeScript type."""
    # Check for array types (e.g., "EntityInfo[]")
    if ftype.endswith("[]"):
        base_type = ftype[:-2]
        inner_type = get_ts_type(base_type, protocol)
        return f"{inner_type}[]"

    # Check built-in types
    if ftype in TYPE_MAP:
        return TYPE_MAP[ftype]

    # Check custom types defined in protocol
    if "types" in protocol and ftype in protocol["types"]:
        return to_pascal_case(ftype)  # Convert to PascalCase

    return ftype

def generate():
    with open(PROTOCOL_PATH) as f:
        protocol = json.load(f)

    lines = [
        "// Auto-generated from protocol.json - do not edit",
        "",
    ]

    # Generate custom types first
    if "types" in protocol:
        for type_name, type_def in protocol["types"].items():
            pascal_name = to_pascal_case(type_name)
            lines.append(f"export interface {pascal_name} {{")
            for field, ftype in type_def.items():
                ts_type = get_ts_type(ftype, protocol)
                lines.append(f"  {field}: {ts_type};")
            lines.append("}")
            lines.append("")

    # Command enum
    lines.append("export const CommandType = {")
    for name, cmd in protocol["commands"].items():
        lines.append(f"  {name}: {cmd['id']},")
    lines.extend([
        "} as const;",
        "",
        "export type CommandType = typeof CommandType[keyof typeof CommandType];",
        "",
    ])

    # Response enum
    lines.append("export const ResponseType = {")
    for name, id in protocol["responses"].items():
        lines.append(f"  {name}: {id},")
    lines.extend([
        "} as const;",
        "",
        "export type ResponseType = typeof ResponseType[keyof typeof ResponseType];",
        "",
    ])

    # Request interfaces
    for name, cmd in protocol["commands"].items():
        if cmd["request"]:
            lines.append(f"export interface {name}Request {{")
            for field, ftype in cmd["request"].items():
                ts_type = get_ts_type(ftype, protocol)
                lines.append(f"  {field}: {ts_type};")
            lines.append("}")
            lines.append("")

    # Response interfaces (deduplicated)
    generated = set()
    for name, cmd in protocol["commands"].items():
        resp = cmd["response"]
        resp_type = resp["type"]
        if resp_type not in ["Ok", "Error"] and "fields" in resp and resp_type not in generated:
            generated.add(resp_type)
            lines.append(f"export interface {resp_type}Response {{")
            for field, ftype in resp["fields"].items():
                ts_type = get_ts_type(ftype, protocol)
                lines.append(f"  {field}: {ts_type};")
            lines.append("}")
            lines.append("")

    OUTPUT_PATH.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT_PATH.write_text("\n".join(lines))
    print(f"Generated {OUTPUT_PATH}")

if __name__ == "__main__":
    generate()