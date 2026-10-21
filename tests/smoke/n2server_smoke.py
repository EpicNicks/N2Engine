#!/usr/bin/env python3
"""N2Server smoke test, one script for Windows and Linux (standard library only; ctest runs it as n2server_smoke).

Runs the real N2Server end to end on loopback:
  - --new-token makes join tokens in a tokens file (the secret is printed once, the file keeps only the hash);
  - --relay and a non-loopback --bind without a tokens file are refused;
  - the server starts on a small project, prints its ready line, and a test client (n2server_smoke_client: the A5
    handshake over UDP) joins as a player and as a spectator, while a client with a wrong token never connects;
  - the server logs the connects and disconnects, and never a token;
  - it stops cleanly: stdin closing (--exit-on-stdin-eof, everywhere), and on POSIX SIGTERM, SIGINT and a SIGHUP that
    re-reads the tokens file.

  python tests/smoke/n2server_smoke.py --server build/bin/N2Server --client build/bin/n2server_smoke_client

Any failed assertion exits with code 1 and a message; the servers' stdout and stderr are printed at the end. See
docs/testing.html (N2Server smoke test).
"""

import argparse
import json
import os
import re
import shutil
import signal
import stat
import subprocess
import sys
import tempfile
import time
import uuid
from pathlib import Path

READY_LINE = re.compile(r"^N2Server ready (?:.* )?port=(\d+)(?: |$)")
TOKEN_LINE = re.compile(r"^token: ([0-9a-f]{64})$", re.MULTILINE)
IS_POSIX = os.name == "posix"


class SmokeFailure(Exception):
    pass


def say(text):
    print(text, flush=True)


def check(condition, message):
    if not condition:
        raise SmokeFailure(message)


def text_of(data):
    return data.decode("utf-8", errors="replace")


class Servers:
    """Starts server processes with stdout and stderr redirected to files in the work folder, and ends what is left."""

    def __init__(self, exe, work):
        self.exe = str(exe)
        self.work = work
        self.procs = []
        self.logs = []

    def start(self, name, args):
        out_path = self.work / (name + ".out")
        err_path = self.work / (name + ".err")
        self.logs.append((name, out_path, err_path))
        with open(out_path, "wb") as out, open(err_path, "wb") as err:
            proc = subprocess.Popen([self.exe] + args, stdin=subprocess.PIPE, stdout=out, stderr=err)
        proc.n2_out = out_path
        proc.n2_err = err_path
        self.procs.append(proc)
        return proc

    def run(self, args, timeout=120):
        """Runs the server to completion (--new-token, a refusal)."""
        return subprocess.run([self.exe] + args, stdin=subprocess.DEVNULL, capture_output=True, timeout=timeout)

    def end_all(self):
        # Only processes this script started. A failure while ending one must not replace the error that got us here.
        for proc in self.procs:
            try:
                if proc.poll() is None:
                    proc.terminate()
                    try:
                        proc.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        proc.kill()
                        proc.wait(timeout=10)
                if proc.stdin is not None:
                    proc.stdin.close()
            except Exception:  # noqa: BLE001
                pass

    def dump(self):
        for name, out_path, err_path in self.logs:
            for label, path in (("stdout", out_path), ("stderr", err_path)):
                try:
                    data = path.read_bytes()
                except OSError:
                    continue
                say("---- %s %s ----" % (name, label))
                say(text_of(data))


def read_text(path):
    try:
        return text_of(Path(path).read_bytes())
    except OSError:
        return ""


def wait_for(predicate, timeout, what, proc=None):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        if proc is not None and proc.poll() is not None:
            value = predicate()
            if value:
                return value
            raise SmokeFailure("the server exited with code %s while waiting for %s" % (proc.returncode, what))
        time.sleep(0.05)
    raise SmokeFailure("timed out after %d s waiting for %s" % (timeout, what))


def wait_ready(proc, timeout=180):
    """The port of the first ready line on the server's stdout."""

    def find():
        for line in read_text(proc.n2_out).splitlines():
            match = READY_LINE.match(line)
            if match:
                return int(match.group(1))
        return None

    return wait_for(find, timeout, "the ready line", proc)


def wait_exit(proc, timeout=60):
    try:
        return proc.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        raise SmokeFailure("the server did not stop within %d s" % timeout)


def make_project(folder):
    """The smallest project a server can run: a project file and an empty scene."""
    scene = "res://scenes/main.scene"
    (folder / "assets" / "scenes").mkdir(parents=True)
    (folder / "assets" / "scenes" / "main.scene").write_text(
        json.dumps({"name": "main", "rootGameObjects": []}, indent=2) + "\n", encoding="utf-8")
    project = {
        "engineVersion": "1.0.0",
        "formatVersion": 1,
        "name": "n2server-smoke",
        "projectId": str(uuid.uuid4()),
        "scenes": [scene],
        "settings": {},
        "startupScene": scene,
    }
    (folder / "project.n2proj").write_text(json.dumps(project, indent=2) + "\n", encoding="utf-8")


def new_token(servers, tokens_file, user, role):
    result = servers.run(["--new-token", user, role, "--tokens-file", str(tokens_file)])
    out = text_of(result.stdout)
    check(result.returncode == 0,
          "--new-token %s %s failed (%d): %s" % (user, role, result.returncode, text_of(result.stderr)))
    match = TOKEN_LINE.search(out)
    check(match is not None, "--new-token printed no 'token: <64 hex digits>' line:\n" + out)
    token = match.group(1)
    check(out.count(token) == 1, "--new-token must print the token exactly once")
    check("sha256:" not in out, "--new-token must not print the hash")
    return token


def run_client(client, port, token, extra=None, timeout=60):
    args = [str(client), "--port", str(port)] + (extra or [])
    return subprocess.run(args, input=(token + "\n").encode("ascii"), capture_output=True, timeout=timeout)


def check_tokens_file(tokens_file, tokens):
    text = tokens_file.read_text(encoding="utf-8")
    entries = json.loads(text)
    check(isinstance(entries, list) and len(entries) == len(tokens),
          "the tokens file should hold %d entries: %s" % (len(tokens), text))
    for entry in entries:
        check(set(entry) == {"user", "role", "hash"}, "an entry has the wrong keys: %s" % sorted(entry))
        check(re.fullmatch(r"sha256:[0-9a-f]{64}", entry["hash"]) is not None, "an entry's hash is malformed")
    for token in tokens:
        check(token not in text, "a token's secret is in the tokens file")
    if IS_POSIX:
        mode = stat.S_IMODE(tokens_file.stat().st_mode)
        check(mode & 0o077 == 0, "the tokens file is not private to its owner: mode %o" % mode)


def server_args(project, tokens_file, extra=None):
    return ["--project", str(project), "--port", "0", "--bind", "127.0.0.1", "--tokens-file", str(tokens_file),
            "--max-clients", "4"] + (extra or [])


def assert_no_secrets(servers, tokens):
    for name, out_path, err_path in servers.logs:
        text = read_text(out_path) + read_text(err_path)
        for token in tokens:
            check(token not in text, "a token's secret reached the output of %s" % name)


def scenario_refusals(servers, project, work):
    say("refusals")
    result = servers.run(["--relay", "--project", str(project)])
    check(result.returncode == 1, "--relay must be refused (exit code 1), got %d" % result.returncode)
    check("not implemented" in text_of(result.stderr), "--relay's refusal must say it is not implemented")
    check(result.stdout == b"", "a refused --relay must print nothing on stdout (no ready line)")

    result = servers.run(["--project", str(project), "--bind", "0.0.0.0"])
    check(result.returncode == 1, "a non-loopback --bind without a tokens file must be refused, got %d" % result.returncode)
    check("Refusing to bind" in text_of(result.stderr), "the refusal must say what it refuses")
    check(result.stdout == b"", "a refused bind must print no ready line")

    result = servers.run(["--project", str(project), "--tokens-flie", "x"])
    check(result.returncode == 1 and "Unknown option" in text_of(result.stderr), "an unknown option must be an error")

    result = servers.run(["--project", str(work / "no-such-project")])
    check(result.returncode == 1, "a missing project must end the server with exit code 1")

    result = servers.run(["--project", str(project), "--tokens-file", str(work / "no-such-tokens.json")])
    check(result.returncode == 1 and "tokens file" in text_of(result.stderr),
          "a missing tokens file must end the server with exit code 1 and name the tokens file")

    result = servers.run(["--help"])
    check(result.returncode == 0 and b"--tokens-file" in result.stdout, "--help must print the usage and exit 0")

    if IS_POSIX:
        loose = work / "loose-tokens.json"
        loose.write_text("[]\n", encoding="utf-8")
        loose.chmod(0o644)
        result = servers.run(["--project", str(project), "--tokens-file", str(loose)])
        check(result.returncode == 1 and "tokens file" in text_of(result.stderr),
              "a tokens file that others can read must be refused on POSIX")
        check(result.stdout == b"", "a refused tokens file must print no ready line")


def scenario_serve(servers, client, project, tokens_file, work, tokens, stop):
    say("serve and stop by " + stop)
    proc = servers.start("serve-" + stop, server_args(project, tokens_file, ["--exit-on-stdin-eof"]))
    port = wait_ready(proc)
    check(0 < port < 65536, "the ready line's port is out of range: %d" % port)
    line = [l for l in read_text(proc.n2_out).splitlines() if READY_LINE.match(l)][0]
    check(line == "N2Server ready port=%d bind=127.0.0.1" % port, "unexpected ready line: " + line)
    err = proc.n2_err

    # The ready line comes only once the server listens, and the log says what it listens on and what it will do
    log = read_text(err)
    check("Listening on 127.0.0.1:%d (UDP), at most 4 client(s)" % port in log, "no 'Listening on' log line")
    check("headless: no renderer, no window, no audio, physics GPU off" in log, "no headless log line")
    check("Loaded scene res://scenes/main.scene" in log, "the scene was not loaded")

    alice, watcher = tokens["alice"], tokens["watcher"]

    result = run_client(client, port, alice, ["--expect-role", "player"])
    check(result.returncode == 0, "the player client failed (%d): %s%s" % (
        result.returncode, text_of(result.stdout), text_of(result.stderr)))
    out = text_of(result.stdout)
    check("connected role=player" in out and "disconnected" in out, "the player client's output: " + out)

    result = run_client(client, port, watcher, ["--expect-role", "spectator"])
    check(result.returncode == 0, "the spectator client failed (%d): %s%s" % (
        result.returncode, text_of(result.stdout), text_of(result.stderr)))
    check("connected role=spectator" in text_of(result.stdout), "the spectator client's output")

    # A token the server does not know never connects: the server says nothing, so the client times out
    result = run_client(client, port, "0" * 64, ["--expect-failure"])
    check(result.returncode == 0 and "failed reason=handshake-timeout" in text_of(result.stdout),
          "a wrong token must not connect: %s%s" % (text_of(result.stdout), text_of(result.stderr)))
    # ...and neither does another protocol id, with a right token
    result = run_client(client, port, alice, ["--expect-failure", "--protocol-id", "12345"])
    check(result.returncode == 0 and "failed reason=handshake-timeout" in text_of(result.stdout),
          "another protocol id must not connect: %s%s" % (text_of(result.stdout), text_of(result.stderr)))

    # The server logged who joined and left (the peer is a loopback address and a port)
    wait_for(lambda: re.search(r"Client connected: user=alice role=player peer=\d+ address=127\.0\.0\.1:\d+ \(1/4\)",
                               read_text(err)), 15, "the player's connect line", proc)
    wait_for(lambda: re.search(r"Client disconnected: user=alice role=player peer=\d+ \(0/4\)", read_text(err)), 20,
             "the player's disconnect line", proc)
    wait_for(lambda: "Client connected: user=watcher role=spectator" in read_text(err), 15,
             "the spectator's connect line", proc)
    wait_for(lambda: "Client disconnected: user=watcher role=spectator" in read_text(err), 20,
             "the spectator's disconnect line", proc)
    check(read_text(err).count("Client connected:") == 2,
          "only the two clients with a good token may be logged as connected")
    check(proc.poll() is None, "the server must still be running")

    if stop == "hangup":
        # A token added while the server runs joins after SIGHUP, not before
        carol = new_token(servers, tokens_file, "carol", "player")
        tokens["carol"] = carol
        result = run_client(client, port, carol, ["--expect-failure"])
        check(result.returncode == 0, "a user added to the file must not join before SIGHUP: " + text_of(result.stdout))
        proc.send_signal(signal.SIGHUP)
        wait_for(lambda: "Reloaded the tokens file: 3 user(s)" in read_text(err), 15, "the reload line", proc)
        result = run_client(client, port, carol, ["--expect-role", "player"])
        check(result.returncode == 0, "a user added to the file must join after SIGHUP: %s%s" % (
            text_of(result.stdout), text_of(result.stderr)))
        check(proc.poll() is None, "SIGHUP must not stop the server")
        proc.send_signal(signal.SIGTERM)
        stop = "sigterm"
    elif stop == "stdin":
        proc.stdin.close()
    elif stop == "sigterm":
        proc.send_signal(signal.SIGTERM)
    elif stop == "sigint":
        proc.send_signal(signal.SIGINT)
    else:
        raise SmokeFailure("unknown stop " + stop)

    code = wait_exit(proc)
    check(code == 0, "the server must stop with exit code 0 when asked to (%s), got %s" % (stop, code))
    log = read_text(err)
    check("Server stopped" in log and "Engine shut down" in log, "the server did not log a clean shutdown:\n" + log)
    if stop == "stdin":
        check("Stdin closed" in log, "no 'Stdin closed' log line")
    else:
        check("Shutdown signal received" in log, "no 'Shutdown signal received' log line")
    check("Stopping: " in log and "session(s) established" in log, "no final counters line")


def main():
    parser = argparse.ArgumentParser(description="N2Server smoke test")
    parser.add_argument("--server", required=True, help="the N2Server executable")
    parser.add_argument("--client", required=True, help="the n2server_smoke_client executable")
    args = parser.parse_args()
    server = Path(args.server).resolve()
    client = Path(args.client).resolve()
    check(server.is_file(), "no such file: %s" % server)
    check(client.is_file(), "no such file: %s" % client)

    work = Path(tempfile.mkdtemp(prefix="n2server-smoke-"))
    servers = Servers(server, work)
    tokens = {}
    try:
        project = work / "project"
        project.mkdir()
        make_project(project)
        tokens_file = work / "tokens.json"

        say("new tokens")
        tokens["alice"] = new_token(servers, tokens_file, "alice", "player")
        tokens["watcher"] = new_token(servers, tokens_file, "watcher", "spectator")
        check(tokens["alice"] != tokens["watcher"], "two tokens must differ")
        check_tokens_file(tokens_file, list(tokens.values()))
        dup = servers.run(["--new-token", "alice", "admin", "--tokens-file", str(tokens_file)])
        check(dup.returncode == 1 and b"already in it" in dup.stderr, "a user already in the file must be refused")
        check(dup.stdout == b"", "a refused --new-token must print no token")
        check_tokens_file(tokens_file, list(tokens.values()))

        scenario_refusals(servers, project, work)
        scenario_serve(servers, client, project, tokens_file, work, tokens, "stdin")
        if IS_POSIX:
            scenario_serve(servers, client, project, tokens_file, work, tokens, "sigterm")
            scenario_serve(servers, client, project, tokens_file, work, tokens, "sigint")
            scenario_serve(servers, client, project, tokens_file, work, tokens, "hangup")
        assert_no_secrets(servers, list(tokens.values()))
        say("N2Server smoke test passed")
        return 0
    except SmokeFailure as failure:
        servers.end_all()
        servers.dump()
        say("FAILED: %s" % failure)
        return 1
    except Exception as failure:  # noqa: BLE001
        servers.end_all()
        servers.dump()
        say("FAILED with an unexpected error: %r" % (failure,))
        return 1
    finally:
        servers.end_all()
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
