#!/usr/bin/env python3
# editor-server/protocol/generators/generate_cpp.py
#
# Declarations only (ids, PROTOCOL version, request and response structs); the server's codecs are hand-written in
# editor-server/include/editor-server/Commands.hpp and tested against protocol.json and the golden vectors.
#
# Usage: generate_cpp.py [--out <path>] [--protocol <protocol.json>]   (default: editor-server/clients/cpp/Protocol.generated.hpp)

import argparse
from pathlib import Path

import sys

# Importing protocol_spec would otherwise leave a __pycache__ folder in the source tree
sys.dont_write_bytecode = True
import protocol_spec  # noqa: E402

SCRIPT_DIR = Path(__file__).parent
PROTOCOL_PATH = protocol_spec.PROTOCOL_PATH
OUTPUT_PATH = Path(__file__).parent.parent.parent / "clients" / "cpp" / "Protocol.generated.hpp"

TYPE_MAP = {
    "uint8": "uint8_t",
    "uint32": "uint32_t",
    "int32": "int32_t",
    "float32": "float",
    "bool": "bool",
    "string": "std::string",
    "bytes": "std::vector<uint8_t>",
    # Column-major (element col * 4 + row)
    "mat4": "std::array<float, 16>",
    # JSON text, exactly as on the wire
    "json": "std::string",
}

def to_pascal_case(name: str) -> str:
    """Convert name to PascalCase."""
    # Already PascalCase or camelCase - just ensure first letter is capital
    if not '_' in name and not ' ' in name:
        return name[0].upper() + name[1:] if name else name
    return ''.join(word.capitalize() for word in name.replace('_', ' ').split())

def get_cpp_type(ftype: str, protocol: dict) -> str:
    """Convert protocol type to C++ type."""
    field_type = protocol_spec.parse_type(ftype, protocol)
    if field_type.kind == "array":
        return f"std::vector<{get_cpp_type(field_type.base, protocol)}>"
    if field_type.kind == "struct":
        return to_pascal_case(field_type.base)
    return TYPE_MAP[field_type.base]

def field_lines(fields: dict, protocol: dict) -> list:
    lines = []
    for field, ftype in fields.items():
        comment = f" // JSON: {ftype[len('json:'):] if ftype.startswith('json:') else 'any'}" if ftype.startswith("json") else ""
        lines.append(f"    {get_cpp_type(ftype, protocol)} {field};{comment}")
    return lines

def generate(output_path=None, protocol_path=None):
    output_path = Path(output_path) if output_path else OUTPUT_PATH
    protocol = protocol_spec.load(Path(protocol_path) if protocol_path else PROTOCOL_PATH)

    lines = [
        "// Auto-generated from protocol.json by generate_cpp.py - do not edit",
        "// Declarations only: the server's codecs are hand-written (editor-server/include/editor-server/Commands.hpp).",
        "#pragma once",
        "",
        "#include <array>",
        "#include <cstdint>",
        "#include <string>",
        "#include <vector>",
        "",
        "namespace N2Engine::Editor::Protocol",
        "{",
        "",
        "// protocol.json's version (major.minor.patch); Hello sends it",
        f"inline constexpr const char *ProtocolVersion = \"{protocol['version']}\";",
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
            lines.extend(field_lines(type_def, protocol))
            lines.append("};")
            lines.append("")

    # Generate command request structs
    lines.append("// Command request structures")
    for name, request in protocol_spec.commands_with_requests(protocol):
        lines.append(f"struct {name}Cmd")
        lines.append("{")
        lines.extend(field_lines(request, protocol))
        lines.append("};")
        lines.append("")

    # Generate response structs (deduplicated)
    lines.append("// Response structures")
    for resp_type, _, fields in protocol_spec.responses(protocol):
        if fields:
            lines.append(f"struct {resp_type}Data")
            lines.append("{")
            lines.extend(field_lines(fields, protocol))
            lines.append("};")
            lines.append("")

    lines.append("} // namespace N2Engine::Editor::Protocol")
    lines.append("")

    # The output is checked in: only rewritten when its content changes. The build tracks a separate stamp file.
    protocol_spec.write_if_changed(output_path, "\n".join(lines))

def main(argv=None):
    parser = argparse.ArgumentParser(description="Generate the C++ protocol declarations from protocol.json")
    parser.add_argument("--out", type=Path, help=f"output file (default: {OUTPUT_PATH})")
    parser.add_argument("--protocol", type=Path, help=f"the spec to read (default: {PROTOCOL_PATH})")
    args = parser.parse_args(argv)
    generate(args.out, args.protocol)

if __name__ == "__main__":
    main()
