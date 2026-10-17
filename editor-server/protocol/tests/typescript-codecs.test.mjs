// The generated TypeScript codecs (clients/web/protocol.generated.ts) against the golden vectors (test-vectors.json),
// which generate_test_vectors.py encoded independently from protocol.json. Node runs the TypeScript with type
// stripping, so this needs no npm packages. run_tests.py runs it twice: on the checked-in client and vectors, and on
// ones generated from fixture-protocol.json (every field type), which it names in N2_PROTOCOL_JSON, N2_PROTOCOL_TS and
// N2_PROTOCOL_VECTORS. By hand: node --experimental-strip-types --test editor-server/protocol/tests/typescript-codecs.test.mjs
import { test } from "node:test"
import * as assert from "node:assert/strict"
import { readFileSync } from "node:fs"
import { pathToFileURL } from "node:url"

const custom = process.env.N2_PROTOCOL_JSON !== undefined
const specUrl = custom ? pathToFileURL(process.env.N2_PROTOCOL_JSON) : new URL("../protocol.json", import.meta.url)
const vectorsUrl = custom ? pathToFileURL(process.env.N2_PROTOCOL_VECTORS) : new URL("../test-vectors.json", import.meta.url)
const clientUrl = custom
  ? pathToFileURL(process.env.N2_PROTOCOL_TS)
  : new URL("../../clients/web/protocol.generated.ts", import.meta.url)

const spec = JSON.parse(readFileSync(specUrl, "utf-8"))
const vectors = JSON.parse(readFileSync(vectorsUrl, "utf-8"))
const protocol = await import(clientUrl.href)
// The tests after the vector ones use protocol.json's own commands
const realProtocol = { skip: custom ? "a fixture protocol" : false }

const hex = (bytes) => Buffer.from(bytes).toString("hex")
const fromHex = (text) => new Uint8Array(Buffer.from(text, "hex"))

// The vectors write bytes fields as hex; the codecs use Uint8Array
function toCodecValues(fields, values) {
  const result = { ...values }
  for (const [name, type] of Object.entries(fields)) {
    if (type === "bytes") result[name] = fromHex(values[name])
  }
  return result
}

function fromCodecValues(fields, values) {
  const result = { ...values }
  for (const [name, type] of Object.entries(fields)) {
    if (type === "bytes") result[name] = hex(values[name])
  }
  return result
}

function responseFields(name) {
  if (name === "Ok") return {}
  if (name === "Error") return { message: "string" }
  for (const command of Object.values(spec.commands)) {
    if (command.response.type === name) return command.response.fields ?? {}
  }
  if (spec.extraResponses?.[name]) return spec.extraResponses[name].fields ?? {}
  throw new Error(`no response ${name} in protocol.json`)
}

test("PROTOCOL_VERSION is protocol.json's version", () => {
  assert.equal(protocol.PROTOCOL_VERSION, spec.version)
  assert.equal(vectors.protocolVersion, spec.version)
})

test("ids match protocol.json", () => {
  for (const [name, command] of Object.entries(spec.commands)) {
    assert.equal(protocol.CommandType[name], Number.parseInt(command.id, 16), name)
    assert.equal(protocol.CommandResponse[name], command.response.type, name)
  }
  for (const [name, id] of Object.entries(spec.responses)) {
    assert.equal(protocol.ResponseType[name], Number.parseInt(id, 16), name)
  }
})

test("every request with fields and every response has a vector and codecs", () => {
  const requests = Object.entries(spec.commands).filter(([, command]) => Object.keys(command.request).length > 0)
  assert.deepEqual(vectors.requests.map((v) => v.command).sort(), requests.map(([name]) => name).sort())
  assert.deepEqual(Object.keys(protocol.RequestCodecs).sort(), requests.map(([name]) => name).sort())
  assert.deepEqual(vectors.responses.map((v) => v.response).sort(), Object.keys(spec.responses).sort())
  assert.deepEqual(Object.keys(protocol.ResponseCodecs).sort(), Object.keys(spec.responses).sort())
})

// Encodes the vector's values and checks the bytes: exactly, unless a field is json, whose text may be written any
// way (key order, spacing), so then the encoding must decode back to the values instead. Decoding the vector's own
// payload must always give its values.
function checkVector(fields, codec, vector) {
  const encoded = codec.encode(toCodecValues(fields, vector.fields))
  const hasJson = Object.values(fields).some((type) => type.startsWith("json"))
  if (hasJson) {
    assert.deepEqual(fromCodecValues(fields, codec.decode(encoded)), vector.fields)
  } else {
    assert.equal(hex(encoded), vector.payload)
  }
  assert.deepEqual(fromCodecValues(fields, codec.decode(fromHex(vector.payload))), vector.fields)
}

for (const vector of vectors.requests) {
  test(`request ${vector.command}: encodes to the vector and decodes back`, () => {
    checkVector(spec.commands[vector.command].request, protocol.RequestCodecs[vector.command], vector)
  })
}

for (const vector of vectors.responses) {
  test(`response ${vector.response}: encodes to the vector and decodes back`, () => {
    checkVector(responseFields(vector.response), protocol.ResponseCodecs[vector.response], vector)
  })
}

// A response with a second shape (FrameUpdate that is not modified: no pixels), vectors of its own
for (const vector of vectors.responseVariants ?? []) {
  test(`response variant ${vector.response}: encodes to the vector and decodes back`, () => {
    checkVector(responseFields(vector.response), protocol.ResponseCodecs[vector.response], vector)
  })
}

// A peer from before a message's optional trailing fields (protocol.json's optionalTrailing) sends a payload that
// ends before them: the decoder gives their defaults instead of throwing
for (const vector of vectors.shortPayloads ?? []) {
  const name = vector.request ?? vector.response
  test(`short payload ${vector.request ? "request" : "response"} ${name}: decodes with the optional trailing fields defaulted`, () => {
    const codec = vector.request ? protocol.RequestCodecs[name] : protocol.ResponseCodecs[name]
    const fields = vector.request ? spec.commands[name].request : responseFields(name)
    assert.deepEqual(fromCodecValues(fields, codec.decode(fromHex(vector.payload))), vector.fields)
  })
}

test("a 1.10 or 1.11 ServerInfo and Hello decode with the optional trailing fields defaulted", realProtocol, () => {
  const info = { protocolVersion: "1.13.0", engineVersion: "0.1", capabilities: ["scene"], projectLoaded: true, readOnly: true, user: "alice", role: "editor" }
  const full = protocol.encodeServerInfoResponse(info)
  assert.deepEqual(protocol.decodeServerInfoResponse(full), info)
  // With empty user and role the two trailing strings are 4 zero bytes each: a 1.11 host's ServerInfo ends before them, a
  // 1.10 host's before readOnly as well
  const bare = protocol.encodeServerInfoResponse({ ...info, user: "", role: "" })
  assert.deepEqual(protocol.decodeServerInfoResponse(bare.subarray(0, bare.length - 8)), { ...info, user: "", role: "" })
  assert.deepEqual(protocol.decodeServerInfoResponse(bare.subarray(0, bare.length - 9)), { ...info, readOnly: false, user: "", role: "" })
  // Only the optional trailing fields may be missing: a payload cut anywhere earlier is still malformed
  assert.throws(() => protocol.decodeServerInfoResponse(bare.subarray(0, bare.length - 10)), RangeError)
  // ... and a string cut short is malformed, not defaulted
  assert.throws(() => protocol.decodeServerInfoResponse(full.subarray(0, full.length - 2)), RangeError)

  const hello = { clientName: "editor", protocolVersion: "1.13.0", token: "t", errorCodes: true, user: "alice" }
  const helloFull = protocol.encodeHelloRequest(hello)
  assert.deepEqual(protocol.decodeHelloRequest(helloFull), hello)
  const helloBare = protocol.encodeHelloRequest({ ...hello, user: "" })
  // A 1.11 client sends no user; a 1.10 client sends no errorCodes either
  assert.deepEqual(protocol.decodeHelloRequest(helloBare.subarray(0, helloBare.length - 4)), { ...hello, user: "" })
  assert.deepEqual(protocol.decodeHelloRequest(helloBare.subarray(0, helloBare.length - 5)), { ...hello, errorCodes: false, user: "" })
})

test("decoders read from a view with an offset (a Buffer slice) and ignore trailing bytes", realProtocol, () => {
  const vector = vectors.responses.find((v) => v.response === "EngineHealth")
  const padded = new Uint8Array(3 + vector.payload.length / 2 + 4)
  padded.set(fromHex(vector.payload), 3)
  const view = padded.subarray(3)
  assert.deepEqual(protocol.decodeEngineHealthResponse(view), vector.fields)
})

test("a string keeps a leading byte order mark (U+FEFF) through encode and decode", realProtocol, () => {
  const value = { clientName: "\uFEFFeditor", protocolVersion: "1.0.0", token: "\uFEFF", errorCodes: true, user: "\uFEFFalice" }
  assert.deepEqual(protocol.decodeHelloRequest(protocol.encodeHelloRequest(value)), value)
  // An Error's message is decoded straight from the payload, as text
  const error = { message: "\uFEFFbad" }
  assert.deepEqual(protocol.decodeErrorResponse(protocol.encodeErrorResponse(error)), error)
})

test("a truncated payload throws a RangeError", realProtocol, () => {
  const vector = vectors.requests.find((v) => v.command === "Hello")
  const payload = fromHex(vector.payload)
  // Hello's last fields (errorCodes, user) are optional trailing, so cutting them whole is an older client's payload;
  // cutting into the user string, or into token, is not
  assert.throws(() => protocol.decodeHelloRequest(payload.subarray(0, payload.length - 2)), RangeError)
  const userBytes = new TextEncoder().encode(vector.fields.user).length
  const tokenEnd = payload.length - 1 - 4 - userBytes
  assert.deepEqual(protocol.decodeHelloRequest(payload.subarray(0, tokenEnd)).errorCodes, false)
  assert.throws(() => protocol.decodeHelloRequest(payload.subarray(0, tokenEnd - 2)), RangeError)
  assert.throws(() => protocol.decodeHelloRequest(new Uint8Array(0)), RangeError)
})

test("the encoder writes the array's length for count, whatever count says", realProtocol, () => {
  const vector = vectors.responses.find((v) => v.response === "EntityList")
  const encoded = protocol.encodeEntityListResponse({ ...vector.fields, count: 99 })
  assert.equal(hex(encoded), vector.payload)
})

test("encoders reject numbers outside a field's integer type", realProtocol, () => {
  assert.throws(() => protocol.encodeSetViewportSizeRequest({ width: 1.5, height: 1 }), RangeError)
  assert.throws(() => protocol.encodeSetViewportSizeRequest({ width: 2 ** 31, height: 1 }), RangeError)
})

test("frames: header then payload; decodeFrame waits for the whole frame", realProtocol, () => {
  const frame = protocol.encodeFrame(protocol.CommandType.Hello, new Uint8Array([1, 2, 3]))
  assert.equal(hex(frame), "0403000000010203")
  assert.equal(protocol.decodeFrame(frame.subarray(0, 7)), null)
  const decoded = protocol.decodeFrame(frame)
  assert.equal(decoded.type, protocol.CommandType.Hello)
  assert.equal(decoded.size, 8)
  assert.equal(hex(decoded.payload), "010203")
})

test("protocol versions are compatible when the major versions match", () => {
  const { major, minor } = protocol.parseProtocolVersion(protocol.PROTOCOL_VERSION)
  assert.equal(protocol.isProtocolCompatible(`${major}.${minor + 5}.0`), true)
  assert.equal(protocol.isProtocolCompatible(`${major + 1}.0.0`), false)
  assert.equal(protocol.isProtocolCompatible("1.1"), false)
  assert.equal(protocol.parseProtocolVersion("x.y.z"), null)
})
