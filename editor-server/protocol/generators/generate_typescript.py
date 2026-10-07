#!/usr/bin/env python3
# editor-server/protocol/generators/generate_typescript.py
#
# The TypeScript client: ids, PROTOCOL_VERSION, interfaces for every message, type and JSON shape, and an encode and
# decode function for every request and response. Framework-free: it uses only Uint8Array, DataView, TextEncoder and
# TextDecoder, so it runs in Node and in a browser. Only erasable TypeScript syntax is emitted (no enums, namespaces
# or parameter properties), so Node can also run it with type stripping, as the protocol's CI tests do.
#
# Usage: generate_typescript.py [--out <path>] [--protocol <protocol.json>]   (default: editor-server/clients/web/protocol.generated.ts)
# Also importable: set OUTPUT_PATH, then call generate() (the Electron frontend's sync-protocol script does).

import argparse
from pathlib import Path

import sys

# Importing protocol_spec would otherwise leave a __pycache__ folder in the source tree
sys.dont_write_bytecode = True
import protocol_spec  # noqa: E402

SCRIPT_DIR = Path(__file__).parent
PROTOCOL_PATH = protocol_spec.PROTOCOL_PATH
OUTPUT_PATH = Path(__file__).parent.parent.parent / "clients" / "web" / "protocol.generated.ts"

TYPE_MAP = {
    "uint8": "number",
    "uint32": "number",
    "int32": "number",
    "float32": "number",
    "bool": "boolean",
    "string": "string",
    "bytes": "Uint8Array",
    "mat4": "Mat4",
}

# The TypeScript type of each JSON shape primitive
JSON_SHAPE_MAP = {
    "string": "string",
    "number": "number",
    "bool": "boolean",
    "json": "unknown",
}

# ProtocolWriter / ProtocolReader method for each primitive
WRITE_METHOD = {
    "uint8": "u8",
    "uint32": "u32",
    "int32": "i32",
    "float32": "f32",
    "bool": "bool",
    "string": "string",
    "bytes": "bytes",
    "mat4": "mat4",
}
READ_METHOD = dict(WRITE_METHOD, bytes="rest")

# Names a generated decoder uses itself, so a field can't be named after them (fields become its local variables)
RESERVED_FIELD_NAMES = {
    "reader", "payload",
    # JavaScript reserved words
    "break", "case", "catch", "class", "const", "continue", "debugger", "default", "delete", "do", "else", "enum",
    "export", "extends", "false", "finally", "for", "function", "if", "import", "in", "instanceof", "new", "null",
    "return", "super", "switch", "this", "throw", "true", "try", "typeof", "var", "void", "while", "with", "yield",
    "let", "static", "implements", "interface", "package", "private", "protected", "public", "await",
}

RUNTIME = '''// ==================== Codec runtime ====================

const textEncoder = new TextEncoder();
const textDecoder = new TextDecoder();

/** Builds a payload: little-endian numbers, uint32-length-prefixed UTF-8 strings */
export class ProtocolWriter {
  private buffer: Uint8Array = new Uint8Array(64);
  private view: DataView = new DataView(this.buffer.buffer);
  private length = 0;

  u8(value: number): void {
    checkInteger(value, 0, 0xff, "uint8");
    this.reserve(1);
    this.view.setUint8(this.length, value);
    this.length += 1;
  }

  u32(value: number): void {
    checkInteger(value, 0, 0xffffffff, "uint32");
    this.reserve(4);
    this.view.setUint32(this.length, value, true);
    this.length += 4;
  }

  i32(value: number): void {
    checkInteger(value, -0x80000000, 0x7fffffff, "int32");
    this.reserve(4);
    this.view.setInt32(this.length, value, true);
    this.length += 4;
  }

  /** Rounded to the nearest float32 */
  f32(value: number): void {
    this.reserve(4);
    this.view.setFloat32(this.length, value, true);
    this.length += 4;
  }

  bool(value: boolean): void {
    this.u8(value ? 1 : 0);
  }

  string(value: string): void {
    const bytes = textEncoder.encode(value);
    this.u32(bytes.length);
    this.bytes(bytes);
  }

  /** JSON.stringify's text, as a string */
  json(value: unknown): void {
    const text = JSON.stringify(value);
    if (text === undefined) {
      throw new TypeError("Value has no JSON representation");
    }
    this.string(text);
  }

  /** 16 numbers, column-major */
  mat4(value: Mat4): void {
    if (value.length !== 16) {
      throw new RangeError(`A mat4 has 16 elements, not ${value.length}`);
    }
    for (const element of value) {
      this.f32(element);
    }
  }

  /** Raw bytes, no length prefix */
  bytes(value: Uint8Array): void {
    this.reserve(value.length);
    this.buffer.set(value, this.length);
    this.length += value.length;
  }

  /** The bytes written so far (a copy) */
  finish(): Uint8Array {
    return this.buffer.slice(0, this.length);
  }

  private reserve(count: number): void {
    const needed = this.length + count;
    if (needed <= this.buffer.length) {
      return;
    }
    let size = this.buffer.length * 2;
    while (size < needed) {
      size *= 2;
    }
    const grown = new Uint8Array(size);
    grown.set(this.buffer.subarray(0, this.length));
    this.buffer = grown;
    this.view = new DataView(grown.buffer);
  }
}

/** Reads a payload; reading past its end throws a RangeError, as the server's BufferReader does */
export class ProtocolReader {
  private readonly data: Uint8Array;
  private readonly view: DataView;
  private offset = 0;

  constructor(data: Uint8Array) {
    this.data = data;
    this.view = new DataView(data.buffer, data.byteOffset, data.byteLength);
  }

  get remaining(): number {
    return this.data.length - this.offset;
  }

  u8(): number {
    this.require(1);
    const value = this.view.getUint8(this.offset);
    this.offset += 1;
    return value;
  }

  u32(): number {
    this.require(4);
    const value = this.view.getUint32(this.offset, true);
    this.offset += 4;
    return value;
  }

  i32(): number {
    this.require(4);
    const value = this.view.getInt32(this.offset, true);
    this.offset += 4;
    return value;
  }

  f32(): number {
    this.require(4);
    const value = this.view.getFloat32(this.offset, true);
    this.offset += 4;
    return value;
  }

  /** Any non-zero byte is true */
  bool(): boolean {
    return this.u8() !== 0;
  }

  string(): string {
    const length = this.u32();
    return textDecoder.decode(this.take(length));
  }

  /** A string holding JSON text, parsed */
  json(): unknown {
    return JSON.parse(this.string());
  }

  mat4(): Mat4 {
    const value: number[] = [];
    for (let i = 0; i < 16; i++) {
      value.push(this.f32());
    }
    return value;
  }

  /** The rest of the payload: a view into it, not a copy */
  rest(): Uint8Array {
    return this.take(this.remaining);
  }

  private take(count: number): Uint8Array {
    this.require(count);
    const value = this.data.subarray(this.offset, this.offset + count);
    this.offset += count;
    return value;
  }

  private require(count: number): void {
    if (count > this.remaining) {
      throw new RangeError(`Malformed payload: read of ${count} bytes with ${this.remaining} remaining`);
    }
  }
}

function checkInteger(value: number, min: number, max: number, type: string): void {
  if (!Number.isInteger(value) || value < min || value > max) {
    throw new RangeError(`${value} is not a ${type}`);
  }
}

function readArray<T>(reader: ProtocolReader, count: number, read: (reader: ProtocolReader) => T): T[] {
  const values: T[] = [];
  for (let i = 0; i < count; i++) {
    values.push(read(reader));
  }
  return values;
}

/** Bytes in a frame header: [type: uint8][payload length: uint32] */
export const FRAME_HEADER_BYTES = 5;

/** A whole frame: the header, then the payload */
export function encodeFrame(type: number, payload: Uint8Array): Uint8Array {
  const writer = new ProtocolWriter();
  writer.u8(type);
  writer.u32(payload.length);
  writer.bytes(payload);
  return writer.finish();
}

/**
 * The first complete frame at the start of data, and its size in bytes; null until all of it has arrived.
 * The payload is a view into data, not a copy.
 */
export function decodeFrame(data: Uint8Array): { type: number; payload: Uint8Array; size: number } | null {
  if (data.length < FRAME_HEADER_BYTES) {
    return null;
  }
  const view = new DataView(data.buffer, data.byteOffset, data.byteLength);
  const length = view.getUint32(1, true);
  const size = FRAME_HEADER_BYTES + length;
  if (data.length < size) {
    return null;
  }
  return { type: view.getUint8(0), payload: data.subarray(FRAME_HEADER_BYTES, size), size };
}

/** major.minor.patch as numbers; null for anything else */
export function parseProtocolVersion(version: string): { major: number; minor: number; patch: number } | null {
  const match = /^(\\d+)\\.(\\d+)\\.(\\d+)$/.exec(version);
  if (!match) {
    return null;
  }
  return { major: Number(match[1]), minor: Number(match[2]), patch: Number(match[3]) };
}

/**
 * Whether a server speaking version can talk to this client: the same major version. A server with a newer minor
 * version has commands this client doesn't know; one with an older minor version lacks some this client knows.
 */
export function isProtocolCompatible(version: string): boolean {
  const theirs = parseProtocolVersion(version);
  const ours = parseProtocolVersion(PROTOCOL_VERSION);
  return theirs !== null && ours !== null && theirs.major === ours.major;
}
'''

def to_pascal_case(name: str) -> str:
    """Convert name to PascalCase."""
    # Already PascalCase or camelCase - just ensure first letter is capital
    if not '_' in name and not ' ' in name:
        return name[0].upper() + name[1:] if name else name
    return ''.join(word.capitalize() for word in name.replace('_', ' ').split())

def get_ts_shape_type(shape: str) -> str:
    element, depth = protocol_spec.split_json_shape(shape)
    return JSON_SHAPE_MAP.get(element, element) + "[]" * depth

def get_ts_type(ftype: str, protocol: dict) -> str:
    """Convert protocol type to TypeScript type."""
    field_type = protocol_spec.parse_type(ftype, protocol)
    if field_type.kind == "array":
        return f"{get_ts_type(field_type.base, protocol)}[]"
    if field_type.kind == "struct":
        return to_pascal_case(field_type.base)
    if field_type.kind == "json":
        return get_ts_shape_type(field_type.json_shape) if field_type.json_shape else "unknown"
    return TYPE_MAP[field_type.base]

def interface(name: str, fields: dict, protocol: dict, doc: str = "") -> list:
    lines = [f"/** {doc} */"] if doc else []
    if not fields:
        return lines + [f"export type {name} = Record<string, never>;", ""]
    lines.append(f"export interface {name} {{")
    for field, ftype in fields.items():
        lines.append(f"  {field}: {get_ts_type(ftype, protocol)};")
    lines.append("}")
    lines.append("")
    return lines

def write_expression(ftype: str, value: str, protocol: dict) -> str:
    """A statement writing value (an expression of the field's type) to `writer`"""
    field_type = protocol_spec.parse_type(ftype, protocol)
    if field_type.kind == "struct":
        return f"write{to_pascal_case(field_type.base)}(writer, {value});"
    if field_type.kind == "json":
        return f"writer.json({value});"
    if field_type.kind == "array":
        element = protocol_spec.parse_type(field_type.base, protocol)
        inner = write_expression(element.raw, "element", protocol)
        return f"for (const element of {value}) {{ {inner} }}"
    return f"writer.{WRITE_METHOD[field_type.base]}({value});"

def read_expression(ftype: str, protocol: dict, count: str = "") -> str:
    """An expression reading the field from `reader`; count names the element count for an array"""
    field_type = protocol_spec.parse_type(ftype, protocol)
    if field_type.kind == "struct":
        return f"read{to_pascal_case(field_type.base)}(reader)"
    if field_type.kind == "json":
        return f"reader.json() as {get_ts_type(ftype, protocol)}"
    if field_type.kind == "array":
        element = protocol_spec.parse_type(field_type.base, protocol)
        if element.kind == "struct":
            return f"readArray(reader, {count}, read{to_pascal_case(element.base)})"
        return f"readArray(reader, {count}, (reader) => {read_expression(element.raw, protocol)})"
    return f"reader.{READ_METHOD[field_type.base]}()"

def check_field_names(fields: dict, where: str) -> None:
    for field in fields:
        if field in RESERVED_FIELD_NAMES:
            raise ValueError(f"{where}.{field}: the TypeScript codecs can't use this field name")

def encoder(function: str, type_name: str, fields: dict, protocol: dict, doc: str) -> list:
    counts = protocol_spec.array_count_fields(fields, protocol)
    parameter = "value" if fields else "_value"
    lines = [f"/** {doc} */", f"export function {function}({parameter}: {type_name}): Uint8Array {{",
             "  const writer = new ProtocolWriter();"]
    for field, ftype in fields.items():
        if field in counts:
            # Always the array's real length, whatever count says
            lines.append(f"  writer.u32(value.{counts[field]}.length);")
        else:
            lines.append(f"  {write_expression(ftype, f'value.{field}', protocol)}")
    lines.append("  return writer.finish();")
    lines.append("}")
    lines.append("")
    return lines

def decoder(function: str, type_name: str, fields: dict, protocol: dict, doc: str) -> list:
    counts = protocol_spec.array_count_fields(fields, protocol)
    arrays = {array: count for count, array in counts.items()}
    parameter = "payload" if fields else "_payload"
    lines = [f"/** {doc} */", f"export function {function}({parameter}: Uint8Array): {type_name} {{"]
    if not fields:
        lines.append("  return {};")
    else:
        lines.append("  const reader = new ProtocolReader(payload);")
        for field, ftype in fields.items():
            lines.append(f"  const {field} = {read_expression(ftype, protocol, arrays.get(field, ''))};")
        lines.append(f"  return {{ {', '.join(fields)} }};")
    lines.append("}")
    lines.append("")
    return lines

def generate(output_path=None, protocol_path=None):
    output_path = Path(output_path) if output_path else OUTPUT_PATH
    protocol = protocol_spec.load(Path(protocol_path) if protocol_path else PROTOCOL_PATH)

    lines = [
        "// Auto-generated from protocol.json by generate_typescript.py - do not edit",
        "",
        "/** protocol.json's version (major.minor.patch); Hello sends it, and the server answers with its own */",
        f"export const PROTOCOL_VERSION = \"{protocol['version']}\";",
        "",
        "// ==================== Types ====================",
        "",
        "/** 16 numbers, column-major (element col * 4 + row) for column vectors: the translation is elements 12, 13, 14 */",
        "export type Mat4 = number[];",
        "",
    ]

    # Custom types
    for type_name, type_def in protocol.get("types", {}).items():
        lines.extend(interface(to_pascal_case(type_name), type_def, protocol))

    # JSON shapes of json fields
    json_types = protocol.get("jsonTypes", {})
    if json_types:
        lines.append("// ==================== JSON shapes (jsonTypes) ====================")
        lines.append("")
    for type_name, shape_fields in json_types.items():
        lines.append(f"export interface {type_name} {{")
        for key, shape in shape_fields.items():
            name, optional = protocol_spec.json_key(key)
            lines.append(f"  {name}{'?' if optional else ''}: {get_ts_shape_type(shape)};")
        lines.append("}")
        lines.append("")

    # Command enum
    lines.append("// ==================== Ids ====================")
    lines.append("")
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
        "/** The response each command answers with when it succeeds (any command may answer Error instead) */",
        "export const CommandResponse = {",
    ])
    for name, cmd in protocol["commands"].items():
        lines.append(f"  {name}: \"{cmd['response']['type']}\",")
    lines.extend([
        "} as const;",
        "",
        "// ==================== Messages ====================",
        "",
    ])

    # Request interfaces
    requests = protocol_spec.commands_with_requests(protocol)
    for name, request in requests:
        check_field_names(request, f"commands.{name}.request")
        lines.extend(interface(f"{name}Request", request, protocol))

    # Response interfaces (deduplicated)
    response_list = protocol_spec.responses(protocol)
    lines.extend(interface("OkResponse", {}, protocol, "Ok has no payload"))
    lines.extend([
        "/** Error's payload is the message's raw UTF-8 bytes (not a length-prefixed string) */",
        "export interface ErrorResponse {",
        "  message: string;",
        "}",
        "",
    ])
    for resp_type, _, fields in response_list:
        if resp_type in protocol_spec.SPECIAL_RESPONSES:
            continue
        check_field_names(fields, f"responses.{resp_type}")
        lines.extend(interface(f"{resp_type}Response", fields, protocol))

    lines.append(RUNTIME)

    # Struct codecs, exported for code building payloads of its own
    lines.append("// ==================== Codecs ====================")
    lines.append("")
    for type_name, type_def in protocol.get("types", {}).items():
        pascal = to_pascal_case(type_name)
        lines.append(f"export function write{pascal}(writer: ProtocolWriter, value: {pascal}): void {{")
        for field, ftype in type_def.items():
            lines.append(f"  {write_expression(ftype, f'value.{field}', protocol)}")
        lines.append("}")
        lines.append("")
        reads = ", ".join(f"{field}: {read_expression(ftype, protocol)}" for field, ftype in type_def.items())
        lines.append(f"export function read{pascal}(reader: ProtocolReader): {pascal} {{")
        lines.append(f"  return {{ {reads} }};")
        lines.append("}")
        lines.append("")

    # Request codecs
    for name, request in requests:
        lines.extend(encoder(f"encode{name}Request", f"{name}Request", request, protocol,
                             f"{name}'s request payload (without the frame header)"))
        lines.extend(decoder(f"decode{name}Request", f"{name}Request", request, protocol,
                             f"Reads {name}'s request payload; bytes after the last field are ignored"))

    # Response codecs
    lines.extend(encoder("encodeOkResponse", "OkResponse", {}, protocol, "Ok's (empty) payload"))
    lines.extend(decoder("decodeOkResponse", "OkResponse", {}, protocol, "Ok has no fields"))
    lines.extend([
        "/** Error's payload: the message's raw UTF-8 bytes */",
        "export function encodeErrorResponse(value: ErrorResponse): Uint8Array {",
        "  return textEncoder.encode(value.message);",
        "}",
        "",
        "/** Error's message: the whole payload as UTF-8 */",
        "export function decodeErrorResponse(payload: Uint8Array): ErrorResponse {",
        "  return { message: textDecoder.decode(payload) };",
        "}",
        "",
    ])
    for resp_type, _, fields in response_list:
        if resp_type in protocol_spec.SPECIAL_RESPONSES:
            continue
        lines.extend(encoder(f"encode{resp_type}Response", f"{resp_type}Response", fields, protocol,
                             f"{resp_type}'s payload (without the frame header)"))
        lines.extend(decoder(f"decode{resp_type}Response", f"{resp_type}Response", fields, protocol,
                             f"Reads {resp_type}'s payload; bytes after the last field are ignored"))

    # Codec tables, keyed like CommandType and ResponseType
    lines.append("/** Each command's request codecs, for commands with request fields */")
    lines.append("export const RequestCodecs = {")
    for name, _ in requests:
        lines.append(f"  {name}: {{ encode: encode{name}Request, decode: decode{name}Request }},")
    lines.extend(["} as const;", ""])
    lines.append("/** Each response's codecs */")
    lines.append("export const ResponseCodecs = {")
    for resp_type, _, _ in response_list:
        lines.append(f"  {resp_type}: {{ encode: encode{resp_type}Response, decode: decode{resp_type}Response }},")
    lines.extend(["} as const;", ""])

    protocol_spec.write_if_changed(output_path, "\n".join(lines))

def main(argv=None):
    parser = argparse.ArgumentParser(description="Generate the TypeScript protocol client from protocol.json")
    parser.add_argument("--out", type=Path, help=f"output file (default: {OUTPUT_PATH})")
    parser.add_argument("--protocol", type=Path, help=f"the spec to read (default: {PROTOCOL_PATH})")
    args = parser.parse_args(argv)
    generate(args.out, args.protocol)

if __name__ == "__main__":
    main()
