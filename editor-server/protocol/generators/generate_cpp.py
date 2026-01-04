#!/usr/bin/env python3
# editor-server/protocol/generators/generate_cpp.py

import json
from pathlib import Path

SCRIPT_DIR = Path(__file__).parent
PROTOCOL_PATH = Path(__file__).parent.parent / "protocol.json"
OUTPUT_PATH = Path(__file__).parent.parent.parent / "clients" / "cpp" / "Protocol.generated.hpp"

TYPE_MAP = {
    "uint8": "uint8_t",
    "uint32": "uint32_t",
    "int32": "int32_t",
    "float32": "float",
    "bool": "bool",
    "string": "std::string",
    "bytes": "std::vector<uint8_t>",
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

def get_cpp_type(ftype: str, protocol: dict) -> str:
    """Convert protocol type to C++ type."""
    # Check for array types (e.g., "EntityInfo[]")
    if ftype.endswith("[]"):
        base_type = ftype[:-2]
        inner_type = get_cpp_type(base_type, protocol)
        return f"std::vector<{inner_type}>"

    # Check built-in types
    if ftype in TYPE_MAP:
        return TYPE_MAP[ftype]

    # Check custom types defined in protocol
    if "types" in protocol and ftype in protocol["types"]:
        return to_pascal_case(ftype)

    return ftype

def generate():
    with open(PROTOCOL_PATH) as f:
        protocol = json.load(f)

    lines = [
        "// Auto-generated from protocol.json - do not edit",
        "#pragma once",
        "",
        "#include <cstdint>",
        "#include <string>",
        "#include <vector>",
        "",
        "namespace N2Engine::Editor::Protocol",
        "{",
        "",
    ]

    # Command enum
    lines.append("enum class CommandType : uint8_t")
    lines.append("{")
    for name, cmd in protocol["commands"].items():
        lines.append(f"    {name} = {cmd['id']},")
    lines.extend([
        "};",
        "",
    ])

    # Response enum
    lines.append("enum class ResponseType : uint8_t")
    lines.append("{")
    for name, id in protocol["responses"].items():
        lines.append(f"    {name} = {id},")
    lines.extend([
        "};",
        "",
    ])

    # Generate custom types
    if "types" in protocol:
        lines.append("// Custom types")
        for type_name, type_def in protocol["types"].items():
            pascal_name = to_pascal_case(type_name)
            lines.append(f"struct {pascal_name}")
            lines.append("{")
            for field, ftype in type_def.items():
                cpp_type = get_cpp_type(ftype, protocol)
                lines.append(f"    {cpp_type} {field};")
            lines.append("};")
            lines.append("")

    # Generate command request structs
    lines.append("// Command request structures")
    for name, cmd in protocol["commands"].items():
        if cmd["request"]:
            lines.append(f"struct {name}Cmd")
            lines.append("{")
            for field, ftype in cmd["request"].items():
                cpp_type = get_cpp_type(ftype, protocol)
                lines.append(f"    {cpp_type} {field};")
            lines.append("};")
            lines.append("")

    # Generate response structs (deduplicated)
    lines.append("// Response structures")
    generated = set()
    for name, cmd in protocol["commands"].items():
        resp = cmd["response"]
        resp_type = resp["type"]
        if resp_type not in ["Ok", "Error"] and "fields" in resp and resp_type not in generated:
            generated.add(resp_type)
            lines.append(f"struct {resp_type}Data")
            lines.append("{")
            for field, ftype in resp["fields"].items():
                cpp_type = get_cpp_type(ftype, protocol)
                lines.append(f"    {cpp_type} {field};")
            lines.append("};")
            lines.append("")

    lines.append("} // namespace N2Engine::Editor::Protocol")

    OUTPUT_PATH.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT_PATH.write_text("\n".join(lines))
    print(f"Generated {OUTPUT_PATH}")

if __name__ == "__main__":
    generate()