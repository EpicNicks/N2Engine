#!/usr/bin/env python3
# editor-server/protocol/generators/protocol_spec.py
"""Reads and checks protocol.json for the generators and the golden-vector writer.

Field types (see protocol.json's "encoding" section for the wire format of each):
  primitives  uint8, uint32, int32, float32, bool, string, bytes, mat4
  json        json or json:<shape>; a string on the wire
  structs     an entry of "types" (vec3, quat, EntityInfo, ...)
  arrays      <struct or primitive>[]; the element count is the uint32 field named "count" before the array
"""

import json
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

PROTOCOL_PATH = Path(__file__).parent.parent / "protocol.json"

PRIMITIVES = ("uint8", "uint32", "int32", "float32", "bool", "string", "bytes", "mat4")
# Shapes a json field (or a jsonTypes key) may name besides the jsonTypes entries
JSON_SHAPE_PRIMITIVES = ("string", "number", "bool", "json")
# Ids a server may later use for pushed event frames: never a command or response
RESERVED_IDS = range(0xC0, 0xFF)
# Responses that aren't declared by a command: Ok has no payload, Error's payload is raw message bytes
SPECIAL_RESPONSES = ("Ok", "Error")

_IDENTIFIER = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


@dataclass(frozen=True)
class FieldType:
    """One field's declared type, parsed"""
    raw: str
    # "primitive", "struct", "json" or "array"
    kind: str
    # The primitive or struct name; for an array, its element's; for json, "json"
    base: str
    # json fields only: the declared shape ("string[]", "HierarchyNode", ...), or None for any JSON value
    json_shape: Optional[str] = None


def load(path: Optional[Path] = None) -> dict:
    """protocol.json, checked (see validate). Key order is kept: fields go on the wire in declared order."""
    with open(path or PROTOCOL_PATH, encoding="utf-8") as f:
        protocol = json.load(f)
    validate(protocol)
    return protocol


def parse_type(raw: str, protocol: dict) -> FieldType:
    if raw == "json":
        return FieldType(raw, "json", "json")
    if raw.startswith("json:"):
        return FieldType(raw, "json", "json", raw[len("json:"):])
    if raw.endswith("[]"):
        element = parse_type(raw[:-2], protocol)
        if element.kind not in ("primitive", "struct") or element.base == "bytes":
            raise ValueError(f"unsupported array element type: {raw}")
        return FieldType(raw, "array", element.base)
    if raw in PRIMITIVES:
        return FieldType(raw, "primitive", raw)
    if raw in protocol.get("types", {}):
        return FieldType(raw, "struct", raw)
    raise ValueError(f"unknown field type: {raw}")


def split_json_shape(shape: str) -> tuple[str, int]:
    """("HierarchyNode", 1) for "HierarchyNode[]": the element shape and how many [] follow it"""
    depth = 0
    while shape.endswith("[]"):
        shape = shape[:-2]
        depth += 1
    return shape, depth


def json_key(key: str) -> tuple[str, bool]:
    """A jsonTypes key without its optional marker, and whether it had one ("name?" is optional)"""
    return (key[:-1], True) if key.endswith("?") else (key, False)


def responses(protocol: dict) -> list[tuple[str, int, Optional[dict]]]:
    """Every response as (name, id, fields): Ok and Error first, then the rest in the order commands first
    declare them. Fields are None for Ok and Error, which commands don't declare."""
    ids = {name: int(value, 16) for name, value in protocol["responses"].items()}
    result = [(name, ids[name], None) for name in SPECIAL_RESPONSES]
    seen = set(SPECIAL_RESPONSES)
    for command in protocol["commands"].values():
        response = command["response"]
        name = response["type"]
        if name in seen:
            continue
        seen.add(name)
        result.append((name, ids[name], response.get("fields", {})))
    return result


def commands_with_requests(protocol: dict) -> list[tuple[str, dict]]:
    return [(name, command["request"]) for name, command in protocol["commands"].items() if command["request"]]


def array_count_fields(fields: dict, protocol: dict) -> dict[str, str]:
    """{count field: array field} for a message's arrays"""
    counts = {}
    names = list(fields)
    for index, (name, raw) in enumerate(fields.items()):
        if parse_type(raw, protocol).kind != "array":
            continue
        if "count" not in names[:index] or fields["count"] != "uint32":
            raise ValueError(f"array field {name} has no uint32 count field before it")
        if "count" in counts:
            raise ValueError(f"arrays {counts['count']} and {name} share one count field")
        counts["count"] = name
    return counts


def validate(protocol: dict) -> None:
    """Raises ValueError for a protocol.json the generators can't encode correctly"""
    errors = []

    version = protocol.get("version", "")
    if not re.fullmatch(r"\d+\.\d+\.\d+", version):
        errors.append(f"version must be major.minor.patch, not {version!r}")

    json_types = protocol.get("jsonTypes", {})

    def check_shape(shape: str, where: str) -> None:
        element, _ = split_json_shape(shape)
        if element not in JSON_SHAPE_PRIMITIVES and element not in json_types:
            errors.append(f"{where}: unknown JSON shape {shape!r}")

    for type_name, shape_fields in json_types.items():
        if not _IDENTIFIER.match(type_name):
            errors.append(f"jsonTypes.{type_name}: not an identifier")
        for key, shape in shape_fields.items():
            name, _ = json_key(key)
            if not _IDENTIFIER.match(name):
                errors.append(f"jsonTypes.{type_name}.{key}: not an identifier")
            check_shape(shape, f"jsonTypes.{type_name}.{key}")

    def check_fields(fields: dict, where: str) -> None:
        names = list(fields)
        for index, (name, raw) in enumerate(fields.items()):
            if not _IDENTIFIER.match(name):
                errors.append(f"{where}.{name}: not an identifier")
            try:
                field_type = parse_type(raw, protocol)
            except ValueError as error:
                errors.append(f"{where}.{name}: {error}")
                continue
            if field_type.kind == "primitive" and raw == "bytes" and index != len(names) - 1:
                errors.append(f"{where}.{name}: a bytes field must be the last field")
            if field_type.json_shape is not None:
                check_shape(field_type.json_shape, f"{where}.{name}")
        try:
            array_count_fields(fields, protocol)
        except ValueError as error:
            errors.append(f"{where}: {error}")

    for type_name, fields in protocol.get("types", {}).items():
        check_fields(fields, f"types.{type_name}")
        for name, raw in fields.items():
            if raw == "bytes" or raw.endswith("[]") or raw.startswith("json"):
                errors.append(f"types.{type_name}.{name}: struct fields have a fixed layout ({raw} isn't allowed)")

    used_ids: dict[int, str] = {}
    for name, command in protocol["commands"].items():
        command_id = int(command["id"], 16)
        if command_id in RESERVED_IDS:
            errors.append(f"command {name}: id {command['id']} is reserved for pushed events")
        if command_id in used_ids:
            errors.append(f"command {name}: id {command['id']} is also {used_ids[command_id]}")
        used_ids[command_id] = name
        check_fields(command["request"], f"commands.{name}.request")

        response = command["response"]
        if response["type"] not in protocol["responses"]:
            errors.append(f"command {name}: response {response['type']} has no id in responses")
        if response["type"] in SPECIAL_RESPONSES and "fields" in response:
            errors.append(f"command {name}: {response['type']} can't declare fields")
        check_fields(response.get("fields", {}), f"commands.{name}.response")

    declared: dict[str, dict] = {}
    for name, command in protocol["commands"].items():
        response = command["response"]
        fields = response.get("fields", {})
        if response["type"] in declared and declared[response["type"]] != fields:
            errors.append(f"command {name}: response {response['type']} is declared with other fields elsewhere")
        declared.setdefault(response["type"], fields)

    response_ids: dict[int, str] = {}
    for name, value in protocol["responses"].items():
        response_id = int(value, 16)
        if response_id in RESERVED_IDS:
            errors.append(f"response {name}: id {value} is reserved for pushed events")
        if response_id in response_ids:
            errors.append(f"response {name}: id {value} is also {response_ids[response_id]}")
        response_ids[response_id] = name
        if name not in SPECIAL_RESPONSES and name not in declared:
            errors.append(f"response {name}: no command answers with it")

    if errors:
        raise ValueError("protocol.json is invalid:\n  " + "\n  ".join(errors))


def write_if_changed(path: Path, content: str) -> bool:
    """Writes content (LF line endings) unless the file already holds it, ignoring line endings, which git may
    have converted. Returns whether it wrote. Keeps an up-to-date output's timestamp, so builds don't rerun."""
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists():
        existing = path.read_bytes().decode("utf-8").replace("\r\n", "\n")
        if existing == content:
            print(f"Up to date: {path}")
            return False
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(content)
    print(f"Generated {path}")
    return True
