"""The editor protocol's frame codec, for the host smoke tests (standard library only).

Frames are [type: 1 byte][payload length: uint32 LE][payload], both ways. Strings are [uint32 byte count][UTF-8].
Numbers are little-endian. See docs/logging-and-editor.html for the protocol.
"""

import socket
import ssl
import struct
import time

# Command and response type bytes the smoke tests use
OK = 0x00
ERROR = 0x01
RENDER_FRAME = 0x01
FRAME_DATA = 0x02
SET_VIEWPORT_SIZE = 0x02
GET_AUDIO = 0x03
HELLO = 0x04
POLL_EVENTS = 0x05
ENTITY_LIST = 0x05
ENTITY_CREATED = 0x06
ENGINE_HEALTH = 0x09
AUDIO_SAMPLES = 0x0A
SERVER_INFO = 0x0B
EVENTS = 0x0C
SCENE_INFO = 0x0D
PROJECT_INFO = 0x0E
HIERARCHY = 0x0F
ENTITY_DATA = 0x10
OPEN_SCENE = 0x25
SAVE_SCENE_TO_FILE = 0x26
GET_HIERARCHY = 0x28
GET_OPEN_SCENE = 0x29
CREATE_ENTITY = 0x30
GET_ALL_ENTITIES = 0x34
CREATE_ENTITY_EX = 0x35
DUPLICATE_ENTITY = 0x38
GET_ENTITY = 0x39
RESCAN_ASSETS = 0x41
GET_ENGINE_HEALTH = 0x50
GET_PROJECT_INFO = 0x70
SHUTDOWN = 0xFF


class SmokeFailure(Exception):
    """A smoke-test assertion failed."""


def check(condition, message):
    if not condition:
        raise SmokeFailure(message)


def u32(value):
    return struct.pack("<I", value)


def i32(value):
    return struct.pack("<i", value)


def wire_string(text):
    data = text.encode("utf-8")
    return u32(len(data)) + data


def hello_payload(client_name, protocol_version, token):
    return wire_string(client_name) + wire_string(protocol_version) + wire_string(token)


class Reader:
    """Reads fields from a payload in order."""

    def __init__(self, payload, offset=0):
        self.payload = payload
        self.offset = offset

    def u32(self):
        (value,) = struct.unpack_from("<I", self.payload, self.offset)
        self.offset += 4
        return value

    def string(self):
        length = self.u32()
        text = self.payload[self.offset:self.offset + length].decode("utf-8")
        self.offset += length
        return text

    def remaining(self):
        return len(self.payload) - self.offset


def connect(port, tls=False, timeout=10.0):
    """Connects to 127.0.0.1:port. The host listens before it prints its ready line, so the first attempt should
    work; a few retries for safety. With tls, completes a handshake that accepts any certificate (the caller pins
    the fingerprint itself, as a trust-on-first-use client does)."""
    last_error = None
    for _ in range(10):
        try:
            raw = socket.create_connection(("127.0.0.1", port), timeout=timeout)
            break
        except OSError as error:
            last_error = error
            time.sleep(0.5)
    else:
        raise SmokeFailure("Editor host never accepted a connection on port %d: %s" % (port, last_error))
    if not tls:
        return Connection(raw)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    context.check_hostname = False
    context.verify_mode = ssl.CERT_NONE
    raw.settimeout(20.0)
    return Connection(context.wrap_socket(raw, server_hostname="n2-editor-host"))


class Connection:
    def __init__(self, sock):
        self.sock = sock

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass

    def send(self, frame_type, payload=b""):
        self.sock.sendall(bytes([frame_type]) + u32(len(payload)) + payload)

    def _read_exact(self, count):
        chunks = []
        got = 0
        while got < count:
            data = self.sock.recv(count - got)
            if not data:
                raise SmokeFailure("Connection closed after %d of %d bytes" % (got, count))
            chunks.append(data)
            got += len(data)
        return b"".join(chunks)

    def read_response(self):
        header = self._read_exact(5)
        (length,) = struct.unpack_from("<I", header, 1)
        return header[0], self._read_exact(length)

    def request(self, frame_type, payload=b""):
        self.send(frame_type, payload)
        return self.read_response()

    def expect(self, frame_type, payload, expected, what):
        """Sends a command and requires the response type `expected`; returns its payload. Error's payload is its
        message, as raw UTF-8."""
        response_type, response = self.request(frame_type, payload)
        assert_response(response_type, response, expected, what)
        return response

    def closed_by_host(self):
        """True once the host has closed the connection (a read returns 0, or the connection was reset)."""
        try:
            return self.sock.recv(1) == b""
        except socket.timeout:
            return False
        except (ConnectionError, ssl.SSLError, OSError):
            return True


def assert_response(response_type, payload, expected, what):
    if response_type != expected:
        raise SmokeFailure("%s: expected response 0x%02X, got 0x%02X: %s" % (
            what, expected, response_type, payload.decode("utf-8", "replace")))
