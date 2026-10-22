#!/usr/bin/env python3
"""World-protocol loopback test (netcode stage B1), one script for Windows and Linux (standard library only; ctest runs
it as n2server_net).

Real processes on real loopback UDP, through the secure handshake:
  - the real N2Server on a project whose scene has three NetworkObjects: a headless client joins, the world matches
    (ids 1 to 3 on both sides), a client whose scene differs refuses to join with a clear error, so does one whose
    registered prefabs differ, and a client whose scene file has other whitespace, key order and line endings but the same
    content still joins;
  - a test host (N2Server's RunServer with hooks, which registers a prefab and runs spawn and despawn commands from
    stdin) with several clients: a spawn reaches the clients that have joined, with its owner; a client that joins later is
    sent the objects spawned and the scene objects destroyed before it; a despawn of a spawned object and of a scene object
    reaches everyone; a spawn of a prefab outside the table is refused.

  python tests/smoke/n2server_net.py --server build/bin/N2Server --host build/bin/n2server_net_host \\
      --client build/bin/n2server_net_client

Any failed assertion exits with code 1 and a message; every process's stdout and stderr are printed at the end. See
docs/testing.html (the world-protocol loopback test) and docs/netcode.html (the network world).
"""

import argparse
import json
import re
import shutil
import subprocess
import sys
import tempfile
import time
import uuid
from pathlib import Path

READY_LINE = re.compile(r"^N2Server ready (?:.* )?port=(\d+)(?: |$)", re.MULTILINE)
TOKEN_LINE = re.compile(r"^token: ([0-9a-f]{64})$", re.MULTILINE)

OBJECT_UUIDS = [
    "00000000-0000-4000-8000-000000000001",
    "00000000-0000-4000-8000-000000000002",
    "00000000-0000-4000-8000-000000000003",
]


class Failure(Exception):
    pass


def say(text):
    print(text, flush=True)


def check(condition, message):
    if not condition:
        raise Failure(message)


def text_of(data):
    # Windows writes text-mode lines with CRLF: compare on LF
    return data.decode("utf-8", errors="replace").replace("\r\n", "\n")


class Tool:
    """A started process: stdout and stderr in files of the work folder, stdin a pipe the script writes to."""

    def __init__(self, exe, args, work, name):
        self.name = name
        self.out_path = work / (name + ".out")
        self.err_path = work / (name + ".err")
        with open(self.out_path, "wb") as out, open(self.err_path, "wb") as err:
            self.proc = subprocess.Popen([str(exe)] + args, stdin=subprocess.PIPE, stdout=out, stderr=err)

    def send(self, line):
        try:
            self.proc.stdin.write((line + "\n").encode("ascii"))
            self.proc.stdin.flush()
        except (OSError, ValueError):
            pass  # the process has ended

    def close_stdin(self):
        try:
            self.proc.stdin.close()
        except (OSError, ValueError):
            pass

    def out(self):
        try:
            return text_of(self.out_path.read_bytes())
        except OSError:
            return ""

    def err(self):
        try:
            return text_of(self.err_path.read_bytes())
        except OSError:
            return ""

    def exited(self):
        return self.proc.poll() is not None

    def wait(self, timeout, what):
        try:
            return self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            raise Failure("%s did not end within %d s (%s)" % (self.name, timeout, what))

    def end(self):
        try:
            if self.proc.poll() is None:
                self.proc.terminate()
                try:
                    self.proc.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    self.proc.kill()
                    self.proc.wait(timeout=10)
            self.close_stdin()
        except Exception:  # noqa: BLE001
            pass


class Run:
    """The tools started by one run, ended and dumped together."""

    def __init__(self, server, host, client, work):
        self.server = server
        self.host = host
        self.client = client
        self.work = work
        self.tools = []
        self.counter = 0

    def start(self, exe, args, name):
        self.counter += 1
        tool = Tool(exe, args, self.work, "%02d-%s" % (self.counter, name))
        self.tools.append(tool)
        return tool

    def end_all(self):
        for tool in self.tools:
            tool.end()

    def dump(self):
        for tool in self.tools:
            for label, text in (("stdout", tool.out()), ("stderr", tool.err())):
                say("---- %s %s ----" % (tool.name, label))
                say(text)


def wait_for(predicate, timeout, what, *watch):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        for tool in watch:
            if tool.exited():
                value = predicate()
                if value:
                    return value
                raise Failure("%s exited with code %s while waiting for %s" % (tool.name, tool.proc.returncode, what))
        time.sleep(0.05)
    raise Failure("timed out after %d s waiting for %s" % (timeout, what))


def wait_line(tool, pattern, timeout, what, *watch):
    """The match of the first line of the tool's stdout that matches `pattern` (a regular expression)."""
    regex = re.compile(pattern, re.MULTILINE)
    return wait_for(lambda: regex.search(tool.out()), timeout, what, tool, *watch)


def game_object(object_uuid, number):
    return {
        "uuid": object_uuid,
        "name": "Object%d" % number,
        "tag": "Untagged",
        "layer": 0,
        "isActive": True,
        "components": [
            # A fixed component UUID: it is part of the scene's content, which must be the same in every project folder
            {"type": "NetworkObject", "data": {"uuid": "1" + object_uuid[1:], "isActive": True}},
        ],
        "children": [],
    }


def make_project(folder, uuids, indent=2, sort_keys=False, crlf=False):
    """A project with one scene whose objects are `uuids`, each with a NetworkObject. The same objects with another
    indent, key order or line ending is the same scene content."""
    scene = "res://scenes/main.scene"
    (folder / "assets" / "scenes").mkdir(parents=True)
    objects = [game_object(object_uuid, number + 1) for number, object_uuid in enumerate(uuids)]
    scene_text = json.dumps({"name": "main", "rootGameObjects": objects}, indent=indent, sort_keys=sort_keys) + "\n"
    if crlf:
        scene_text = scene_text.replace("\n", "\r\n")
    (folder / "assets" / "scenes" / "main.scene").write_bytes(scene_text.encode("utf-8"))
    project = {
        "engineVersion": "1.0.0",
        "formatVersion": 1,
        "name": "n2server-net",
        "projectId": str(uuid.uuid4()),
        "scenes": [scene],
        "settings": {},
        "startupScene": scene,
    }
    (folder / "project.n2proj").write_text(json.dumps(project, indent=2) + "\n", encoding="utf-8")


def make_tokens(run, tokens_file, count):
    tokens = []
    for number in range(count):
        result = subprocess.run(
            [str(run.server), "--new-token", "player%d" % number, "player", "--tokens-file", str(tokens_file)],
            stdin=subprocess.DEVNULL, capture_output=True, timeout=120)
        check(result.returncode == 0, "--new-token failed: " + text_of(result.stderr))
        match = TOKEN_LINE.search(text_of(result.stdout))
        check(match is not None, "--new-token printed no token line")
        tokens.append(match.group(1))
    return tokens


def server_args(project, tokens_file):
    return ["--project", str(project), "--port", "0", "--bind", "127.0.0.1", "--tokens-file", str(tokens_file),
            "--max-clients", "8"]


def start_server(run, exe, project, tokens_file, name, extra=None):
    tool = run.start(exe, server_args(project, tokens_file) + (extra or []), name)
    match = wait_line(tool, READY_LINE.pattern, 180, "the ready line of " + name, tool)
    return tool, int(match.group(1))


def start_client(run, port, token, project, name, extra=None):
    tool = run.start(run.client, ["--project", str(project), "--port", str(port)] + (extra or []), name)
    tool.send(token)
    return tool


def stop_client(tool):
    tool.send("quit")
    code = tool.wait(60, "after quit")
    check(code == 0, "%s ended with code %s after quit:\n%s%s" % (tool.name, code, tool.out(), tool.err()))
    check("left" in tool.out().splitlines(), "%s did not say it left:\n%s" % (tool.name, tool.out()))


def expect_refusal(run, port, token, project, name, reason, extra=None):
    tool = start_client(run, port, token, project, name, ["--expect-refusal"] + (extra or []))
    code = tool.wait(120, "a refused client ends by itself")
    check(code == 0, "%s was expected to refuse and end with code 0, got %s:\n%s%s" % (name, code, tool.out(), tool.err()))
    check(("refused reason=%s" % reason) in tool.out().splitlines(),
          "%s: no 'refused reason=%s' line:\n%s%s" % (name, reason, tool.out(), tool.err()))
    check("Refusing to join" in tool.err(), "%s: the error text is missing on stderr:\n%s" % (name, tool.err()))
    check("joined client=" not in tool.out(), "%s must not have joined" % name)
    return tool


def objects_line(tool, timeout=30):
    """Asks the client for its object ids and returns them."""
    pattern = re.compile(r"^objects count=(\d+) ids=([0-9,]*)$", re.MULTILINE)
    before = len(pattern.findall(tool.out()))
    tool.send("list")
    found = wait_for(lambda: pattern.findall(tool.out()) if len(pattern.findall(tool.out())) > before else None,
                     timeout, "the object list of " + tool.name, tool)
    return [int(number) for number in found[before][1].split(",") if number]


def scenario_real_server(run, work, tokens):
    say("the real N2Server: join, refusals")
    project = work / "project-a"
    project.mkdir()
    make_project(project, OBJECT_UUIDS)
    tokens_file = work / "tokens.json"
    server, port = start_server(run, run.server, project, tokens_file, "n2server", ["--exit-on-stdin-eof"])
    log = server.err()
    check("Network world: 3 scene object(s) with a NetworkObject" in log, "the server did not match three scene objects")
    check("Network world: 0 prefab(s) in the prefab table" in log, "the server's prefab table is not empty")

    # An honest client (without the test prefab, which N2Server does not have) joins and sees the three scene objects
    client = start_client(run, port, tokens[0], project, "joined-a", ["--no-crate"])
    wait_line(client, r"^joined client=1 objects=3 spawned=0$", 120, "the join", client, server)
    check(objects_line(client) == [1, 2, 3], "the client's object ids are not 1, 2, 3")
    wait_for(lambda: re.search(r"Network client joined the world: client=1 peer=\d+", server.err()), 20,
             "the server's join line", server)

    # The same scene content in another folder, with other whitespace, key order and line endings: it still joins
    project_copy = work / "project-copy"
    project_copy.mkdir()
    make_project(project_copy, OBJECT_UUIDS, indent=4, sort_keys=True, crlf=True)
    twin = start_client(run, port, tokens[1], project_copy, "joined-copy", ["--no-crate"])
    wait_line(twin, r"^joined client=2 objects=3 spawned=0$", 120, "the join of the reformatted scene", twin, server)
    check(objects_line(twin) == [1, 2, 3], "the reformatted scene's object ids are not 1, 2, 3")
    stop_client(twin)

    # A scene with an object more: the scene content differs, and the client refuses with a clear error
    project_other = work / "project-other"
    project_other.mkdir()
    make_project(project_other, OBJECT_UUIDS + ["00000000-0000-4000-8000-000000000004"])
    other = expect_refusal(run, port, tokens[2], project_other, "refused-scene", "scene-content", ["--no-crate"])
    check("res://scenes/main.scene" in other.err(), "the refusal does not name the scene:\n" + other.err())

    # The same scene but the client registers a prefab the server does not have: the registries differ
    expect_refusal(run, port, tokens[3], project, "refused-registry", "registry")

    # The server saw the refusals as refusals, kept running, and the first client is still in
    wait_for(lambda: "is dropped: it refused to join (scene-content)" in server.err() and
             "is dropped: it refused to join (registry)" in server.err(), 30, "the refusals in the server's log", server)
    check(not server.exited(), "the server must still be running")
    check(tokens[0] not in server.err() + server.out(), "a token reached the server's output")
    stop_client(client)

    # Ending: stdin closes, the server stops cleanly
    server.close_stdin()
    code = server.wait(60, "after stdin closed")
    check(code == 0, "the server must stop with exit code 0, got %s" % code)
    log = server.err()
    check("Server stopped" in log and "client(s) joined the world" in log, "no clean shutdown in the server's log")
    check("Network client left the world: client=1" in log, "the server did not log the client leaving")
    for token in tokens:
        check(token not in log + server.out(), "a token's secret reached the server's output")


def scenario_host(run, work, tokens):
    say("the test host: spawn, despawn, late joiners")
    project = work / "project-host"
    project.mkdir()
    make_project(project, OBJECT_UUIDS)
    tokens_file = work / "tokens.json"
    host, port = start_server(run, run.host, project, tokens_file, "host")
    check("Network world: 1 prefab(s) in the prefab table" in host.err(), "the host's table does not hold the test prefab")

    first = start_client(run, port, tokens[4], project, "first")
    wait_line(first, r"^joined client=1 objects=3 spawned=0$", 120, "the first client's join", first, host)

    # A spawn reaches the client that joined
    host.send("spawn Crate")
    wait_line(host, r"^spawned id=4$", 30, "the host's spawn", host)
    wait_line(first, r"^spawn id=4 prefab=Crate owner=0$", 30, "the spawn at the first client", first, host)

    # A second client joins after it: the join state holds the spawned object
    second = start_client(run, port, tokens[5], project, "second")
    wait_line(second, r"^spawn id=4 prefab=Crate owner=0$", 120, "the spawn in the second client's join state", second, host)
    wait_line(second, r"^joined client=2 objects=4 spawned=1$", 30, "the second client's join", second, host)

    # A spawn with an owner reaches both
    host.send("spawn Crate 1")
    wait_line(host, r"^spawned id=5$", 30, "the host's second spawn", host)
    for client in (first, second):
        wait_line(client, r"^spawn id=5 prefab=Crate owner=1$", 30, "the owned spawn at " + client.name, client, host)

    # A prefab outside the table, an owner that has not joined, and an id that is not there: refused, nothing sent
    host.send("spawn NoSuchPrefab")
    wait_line(host, r"^spawn-failed .*prefab table", 30, "the refusal of an unknown prefab", host)
    host.send("spawn Crate 77")
    wait_line(host, r"^spawn-failed .*has not joined", 30, "the refusal of an owner that has not joined", host)
    host.send("despawn 99")
    wait_line(host, r"^despawned id=99 ok=0$", 30, "the refusal of an unknown id", host)

    # A despawn of a spawned object, then of a scene object, reaches everyone
    host.send("despawn 4")
    wait_line(host, r"^despawned id=4 ok=1$", 30, "the host's despawn", host)
    host.send("despawn 2")
    wait_line(host, r"^despawned id=2 ok=1$", 30, "the host's despawn of a scene object", host)
    for client in (first, second):
        wait_line(client, r"^despawn id=4$", 30, "the despawn of 4 at " + client.name, client, host)
        wait_line(client, r"^despawn id=2$", 30, "the despawn of 2 at " + client.name, client, host)
        check(objects_line(client) == [1, 3, 5], "%s does not hold objects 1, 3 and 5" % client.name)

    # A late joiner is told what happened before it: the scene object that is gone, the object that is live
    third = start_client(run, port, tokens[6], project, "third")
    wait_line(third, r"^joined client=3 objects=3 spawned=1$", 120, "the late joiner's join", third, host)
    out = third.out()
    check(out.index("despawn id=2") < out.index("spawn id=5 prefab=Crate owner=1") < out.index("joined client=3"),
          "the join state is not in order (destroyed scene objects, live spawns, the end):\n" + out)
    check("spawn id=4" not in out, "the late joiner was told of an object that was despawned")
    check(objects_line(third) == [1, 3, 5], "the late joiner does not hold objects 1, 3 and 5")

    host.send("joined")
    wait_line(host, r"^joined count=3 ids=1,2,3$", 30, "the host's list of joined clients", host)

    # A client whose registry differs (no test prefab) and one whose scene differs refuse; the others are not disturbed
    expect_refusal(run, port, tokens[7], project, "refused-registry-host", "registry", ["--no-crate"])
    project_other = work / "project-host-other"
    project_other.mkdir()
    make_project(project_other, OBJECT_UUIDS[:2])
    expect_refusal(run, port, tokens[7], project_other, "refused-scene-host", "scene-content")
    host.send("spawn Crate")
    wait_line(host, r"^spawned id=6$", 30, "the host's spawn after the refusals", host)
    for client in (first, second, third):
        wait_line(client, r"^spawn id=6 prefab=Crate owner=0$", 30, "the spawn after the refusals at " + client.name, client, host)

    for client in (first, second, third):
        stop_client(client)
    host.send("quit")
    code = host.wait(60, "after quit")
    check(code == 0, "the host must stop with exit code 0, got %s" % code)
    check("Server stopped" in host.err(), "no clean shutdown in the host's log")
    for token in tokens:
        check(token not in host.err() + host.out(), "a token's secret reached the host's output")


def main():
    parser = argparse.ArgumentParser(description="N2Server world-protocol loopback test")
    parser.add_argument("--server", required=True, help="the N2Server executable")
    parser.add_argument("--host", required=True, help="the n2server_net_host executable")
    parser.add_argument("--client", required=True, help="the n2server_net_client executable")
    args = parser.parse_args()
    paths = {}
    for key in ("server", "host", "client"):
        path = Path(getattr(args, key)).resolve()
        check(path.is_file(), "no such file: %s" % path)
        paths[key] = path

    work = Path(tempfile.mkdtemp(prefix="n2server-net-"))
    run = Run(paths["server"], paths["host"], paths["client"], work)
    try:
        tokens = make_tokens(run, work / "tokens.json", 8)
        scenario_real_server(run, work, tokens)
        scenario_host(run, work, tokens)
        say("N2Server world-protocol loopback test passed")
        return 0
    except Failure as failure:
        run.end_all()
        run.dump()
        say("FAILED: %s" % failure)
        return 1
    except Exception as failure:  # noqa: BLE001
        run.end_all()
        run.dump()
        say("FAILED with an unexpected error: %r" % (failure,))
        return 1
    finally:
        run.end_all()
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
