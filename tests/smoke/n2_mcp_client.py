"""A minimal MCP client over stdio for the n2-mcp bridge smoke test (standard library only).

MCP over stdio is newline-delimited JSON-RPC 2.0: one JSON object per line in each direction. This client speaks the
2025-11-25 revision (initialize, notifications/initialized, tools/list, tools/call) and declares no capabilities, so
the bridge can never ask it to confirm anything (an irreversible tool is then refused, which the smoke test checks).
"""

import json
import os
import queue
import subprocess
import threading
import time

from n2_frames import SmokeFailure


class ToolResult:
    """A tools/call result: is_error, the text blocks joined, and the structuredContent ({source, untrusted, data...})."""

    def __init__(self, raw):
        self.raw = raw
        self.is_error = bool(raw.get("isError"))
        self.text = "\n".join(block.get("text", "") for block in raw.get("content", []) if block.get("type") == "text")
        self.structured = raw.get("structuredContent") or {}

    @property
    def data(self):
        return self.structured.get("data")

    def __str__(self):
        return ("ERROR " if self.is_error else "ok ") + self.text[:300].replace("\n", " ")


class McpClient:
    """Runs `node <bundle> <args>` and talks MCP to it. stderr goes to a file (read it with stderr_text())."""

    def __init__(self, node, bundle, args, env, work, name):
        self.name = name
        self.err_path = work / (name + ".bridge.err")
        self.next_id = 1
        self.lines = queue.Queue()
        self.stdout_log = []
        with open(self.err_path, "wb") as err:
            self.proc = subprocess.Popen([node, str(bundle)] + args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                         stderr=err, env=env)
        threading.Thread(target=self._pump, daemon=True).start()

    def _pump(self):
        for line in self.proc.stdout:
            self.lines.put(line)
        self.lines.put(None)

    def stderr_text(self):
        return self.err_path.read_bytes().decode("utf-8", "replace")

    def _send(self, message):
        self.proc.stdin.write((json.dumps(message) + "\n").encode("utf-8"))
        self.proc.stdin.flush()

    def notify(self, method, params=None):
        self._send({"jsonrpc": "2.0", "method": method, **({"params": params} if params is not None else {})})

    def request(self, method, params=None, timeout=90.0):
        """Sends a request and returns its result; a JSON-RPC error raises SmokeFailure. Messages that are not the
        answer (notifications) are skipped."""
        request_id = self.next_id
        self.next_id += 1
        self._send({"jsonrpc": "2.0", "id": request_id, "method": method, **({"params": params} if params is not None else {})})
        deadline = time.monotonic() + timeout
        while True:
            try:
                line = self.lines.get(timeout=max(0.1, deadline - time.monotonic()))
            except queue.Empty:
                line = ""
            if line is None:
                raise SmokeFailure("The bridge (%s) closed its stdout during %s (exit code %s)" % (
                    self.name, method, self.proc.poll()))
            if not line:
                if time.monotonic() >= deadline:
                    raise SmokeFailure("The bridge (%s) didn't answer %s within %ds" % (self.name, method, timeout))
                continue
            message = json.loads(line)
            self.stdout_log.append(message)
            if message.get("id") == request_id and "method" not in message:
                if "error" in message:
                    raise SmokeFailure("%s: JSON-RPC error %s" % (method, message["error"]))
                return message.get("result", {})

    def initialize(self):
        result = self.request("initialize", {"protocolVersion": "2025-11-25", "capabilities": {},
                                             "clientInfo": {"name": "n2-smoke", "version": "0"}})
        self.notify("notifications/initialized")
        return result

    def list_tools(self):
        return [tool["name"] for tool in self.request("tools/list", {}).get("tools", [])]

    def call(self, tool, arguments=None):
        return ToolResult(self.request("tools/call", {"name": tool, "arguments": arguments or {}}))

    def close(self, wait=15.0):
        """Closes the bridge's stdin (a client going away) and waits for it to exit; kills it if it doesn't. Returns the
        exit code."""
        try:
            self.proc.stdin.close()
        except OSError:
            pass
        try:
            return self.proc.wait(timeout=wait)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait(timeout=10)
            raise SmokeFailure("The bridge (%s) didn't exit within %ds of its stdin closing" % (self.name, wait))

    def kill(self):
        if self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait(timeout=10)


def bridge_env(**extra):
    """The environment for a bridge: the current one, minus a stray N2ENGINE_HOST, plus `extra`."""
    env = {k: v for k, v in os.environ.items() if k != "N2ENGINE_HOST"}
    env.update(extra)
    return env
