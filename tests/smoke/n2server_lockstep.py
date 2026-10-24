#!/usr/bin/env python3
"""Lockstep relay soak (netcode stage C3), one script for Windows and Linux (standard library only; ctest runs it as
n2server_lockstep).

Real processes on real loopback UDP, through the secure handshake: the real `N2Server --relay` and headless clients
(n2server_lockstep_client: a LockstepRuntime running the C2 replay scene from the confirmed ticks):
  - a project whose network.preset is not deterministic is refused by --relay (exit 1, no ready line);
  - two clients play 400 ticks: the relay seats them, starts the session, and both print the same state hash at every
    checkpoint; each recorded its confirmed inputs as an n2replay file, and the C2 golden runner (n2_replay, when given)
    replays that file to the very hashes the live clients printed;
  - an injected divergence (one client reports a flipped bit from tick 100 on): the relay compares the roots, stops the
    session, the two clients bisect against each other through the relay, find the field, write an n2desync report with
    the input log, and stop; the relay goes back to waiting for the next session;
  - a client that hangs (it stops reading and sending): the relay drops its slot after the stall timeout, the other client
    applies the drop and plays on to the end.

  python tests/smoke/n2server_lockstep.py --server build/bin/N2Server --client build/bin/n2server_lockstep_client \\
      [--replay build/bin/n2_replay] [--require-deterministic]

The Deterministic preset refuses to start without Jolt in a strict-FP build. Without --require-deterministic a server that
refuses for that reason makes the run print SKIPPED and exit 0 (a build that cannot have the preset cannot run this test);
with it, that is a failure (the Linux CI legs that build Jolt with N2ENGINE_STRICT_FP pass it). Any failed assertion exits
with code 1 and a message; every process's stdout and stderr are printed at the end.
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
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parent))
from n2server_net import Failure, READY_LINE, Tool, check, make_tokens, say, text_of, wait_for, wait_line  # noqa: E402

LAYOUT = [
    {"name": "move", "kind": "vector2", "bits": 8, "range": 1, "signed": True},
    {"name": "jump", "kind": "button"},
    {"name": "fire", "kind": "button"},
]

HASH_LINE = re.compile(r"^hash tick=(\d+) (0x[0-9a-f]+)$", re.MULTILINE)
REPLAY_LINE = re.compile(r"\[REPLAY\] \S+ tick (\d+) (0x[0-9a-f]+)")


class Skip(Exception):
    pass


def make_project(folder, deterministic):
    scene = "res://scenes/main.scene"
    (folder / "assets" / "scenes").mkdir(parents=True)
    (folder / "assets" / "scenes" / "main.scene").write_text(
        json.dumps({"name": "main", "rootGameObjects": []}, indent=2) + "\n", encoding="utf-8")
    settings = {}
    if deterministic:
        settings = {
            "network": {"preset": "deterministic"},
            "physics": {"gpu": "off", "deterministic": True, "backend": "jolt", "fixedTimestep": 1.0 / 60.0},
            "math": {"simdTier": "scalar"},
            "determinism": {"luaSeed": 12345},
        }
    project = {
        "engineVersion": "1.0.0",
        "formatVersion": 1,
        "name": "n2server-lockstep",
        "projectId": str(uuid.uuid4()),
        "scenes": [scene],
        "settings": settings,
        "startupScene": scene,
    }
    (folder / "project.n2proj").write_text(json.dumps(project, indent=2) + "\n", encoding="utf-8")


class Harness:
    def __init__(self, args, work):
        self.args = args
        self.work = work
        self.tools = []
        self.counter = 0

    def start(self, exe, argv, name):
        self.counter += 1
        tool = Tool(exe, argv, self.work, "%02d-%s" % (self.counter, name))
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

    def start_relay(self, project, tokens_file, name, extra=None):
        argv = ["--relay", "--project", str(project), "--port", "0", "--bind", "127.0.0.1", "--tokens-file",
                str(tokens_file), "--relay-layout", str(self.work / "layout.json"), "--exit-on-stdin-eof"] + (extra or [])
        tool = self.start(self.args.server, argv, name)
        deadline = time.monotonic() + 240
        while time.monotonic() < deadline:
            match = READY_LINE.search(tool.out())
            if match:
                return tool, int(match.group(1))
            if tool.exited():
                err = tool.err()
                if "refused to start" in err and not self.args.require_deterministic:
                    raise Skip(err.strip().splitlines()[-1][:300])
                raise Failure("the relay exited with code %s before its ready line:\n%s" % (tool.proc.returncode, err))
            time.sleep(0.05)
        raise Failure("timed out waiting for the relay's ready line")

    def start_client(self, port, token, name, extra=None):
        argv = ["--port", str(port), "--layout-file", str(self.work / "layout.json")] + (extra or [])
        tool = self.start(self.args.client, argv, name)
        tool.send(token)
        return tool


def hashes_of(tool):
    return {int(tick): value for tick, value in HASH_LINE.findall(tool.out())}


def scenario_refusal(h, tokens_file):
    say("--relay on a project that is not deterministic is refused")
    project = h.work / "project-plain"
    project.mkdir()
    make_project(project, False)
    result = subprocess.run([str(h.args.server), "--relay", "--project", str(project), "--relay-layout",
                             str(h.work / "layout.json")], stdin=subprocess.DEVNULL, capture_output=True, timeout=120)
    check(result.returncode == 1, "--relay on a plain project must exit 1, got %d" % result.returncode)
    check("not deterministic" in text_of(result.stderr), "the refusal must say why: " + text_of(result.stderr))
    check(result.stdout == b"", "a refused --relay prints no ready line")
    result = subprocess.run([str(h.args.server), "--relay-players", "3", "--project", str(project)],
                            stdin=subprocess.DEVNULL, capture_output=True, timeout=60)
    check(result.returncode == 1 and "only apply with --relay" in text_of(result.stderr),
          "--relay-players without --relay must be an error")


def scenario_session(h, project, tokens, tokens_file):
    say("two clients, 400 ticks, same hashes, replay of the recording")
    server, port = h.start_relay(project, tokens_file, "relay-session")
    a_out = h.work / "a.n2replay.json"
    b_out = h.work / "b.n2replay.json"
    a = h.start_client(port, tokens[0], "client-a", ["--ticks", "400", "--replay-out", str(a_out)])
    b = h.start_client(port, tokens[1], "client-b", ["--ticks", "400", "--replay-out", str(b_out)])
    for client in (a, b):
        code = client.wait(120, "playing 400 ticks")
        check(code == 0, "%s ended with code %s:\n%s%s" % (client.name, code, client.out(), client.err()))
        check(re.search(r"^done ticks=\d+$", client.out(), re.MULTILINE), "%s did not finish:\n%s" % (client.name, client.out()))
    slots = sorted([re.search(r"slot=(\d)", a.out()).group(1), re.search(r"slot=(\d)", b.out()).group(1)])
    check(slots == ["0", "1"], "the clients must take slots 0 and 1, got %s" % slots)
    ha, hb = hashes_of(a), hashes_of(b)
    common = sorted(set(ha) & set(hb))
    check(len(common) >= 12, "the clients share only %d checkpoints" % len(common))
    for tick in common:
        check(ha[tick] == hb[tick], "the clients' hashes differ at tick %d: %s vs %s" % (tick, ha[tick], hb[tick]))
    check(a_out.read_text() == b_out.read_text(), "the two recorded input logs differ")
    say("  %d checkpoints agree" % len(common))

    if h.args.replay and Path(h.args.replay).exists():
        result = subprocess.run([str(h.args.replay), str(a_out)], capture_output=True, timeout=300)
        check(result.returncode == 0, "n2_replay failed on the recording:\n" + text_of(result.stdout) + text_of(result.stderr))
        replayed = {int(t): v for t, v in REPLAY_LINE.findall(text_of(result.stdout))}
        check(len(replayed) >= 12, "n2_replay printed only %d hashes:\n%s" % (len(replayed), text_of(result.stdout)))
        for tick, value in replayed.items():
            check(ha.get(tick) == value, "the replay's hash at tick %d (%s) is not the live one (%s)" % (tick, value, ha.get(tick)))
        say("  the C2 golden runner replays the recording to the live hashes (%d checkpoints)" % len(replayed))
    else:
        say("  (no n2_replay given: the replay of the recording was not run)")

    wait_for(lambda: "the session is over; waiting for the next one" in server.err(), 20, "the relay's session end", server)
    server.close_stdin()
    check(server.wait(60, "after stdin closed") == 0, "the relay did not stop cleanly")


def scenario_desync(h, project, tokens, tokens_file):
    say("an injected divergence is found, bisected and reported by both peers; the session stops")
    server, port = h.start_relay(project, tokens_file, "relay-desync")
    reports = [h.work / "report-a.json", h.work / "report-b.json"]
    a = h.start_client(port, tokens[0], "desync-a", ["--ticks", "2000", "--report", str(reports[0])])
    b = h.start_client(port, tokens[1], "desync-b", ["--ticks", "2000", "--diverge-at-tick", "100", "--report", str(reports[1])])
    for client in (a, b):
        code = client.wait(120, "stopping on the desync")
        check(code == 0, "%s ended with code %s:\n%s%s" % (client.name, code, client.out(), client.err()))
        out = client.out()
        check(re.search(r"^stopped reason=desync tick=120$", out, re.MULTILINE), "%s: no desync stop at tick 120:\n%s" % (client.name, out))
        check(re.search(r"^divergence kind=field-differs object=1 field=0 rounds=\d+$", out, re.MULTILINE),
              "%s: the bisection did not name object 1 field 0:\n%s" % (client.name, out))
        check("report path=" in out, "%s did not write its report" % client.name)
        check("done ticks" not in out, "%s kept playing after the desync" % client.name)
    for path in reports:
        report = json.loads(path.read_text(encoding="utf-8"))
        check(report.get("format") == "n2desync", "%s is not an n2desync report" % path.name)
        check(report.get("tick") == 120, "%s names tick %s, expected 120" % (path.name, report.get("tick")))
        check(report.get("inputLog", {}).get("format") == "n2replay", "%s carries no input log" % path.name)
        check(report.get("trees"), "%s carries no checkpoint trees" % path.name)
    wait_for(lambda: "the session was stopped at tick 120" in server.err(), 20, "the relay's stop line", server)
    wait_for(lambda: "the session is over; waiting for the next one" in server.err(), 30, "the relay's session end", server)
    server.close_stdin()
    check(server.wait(60, "after stdin closed") == 0, "the relay did not stop cleanly")


def scenario_stall(h, project, tokens, tokens_file):
    say("a hung client is dropped; the other plays on")
    server, port = h.start_relay(project, tokens_file, "relay-stall", ["--relay-stall-ms", "1500"])
    a = h.start_client(port, tokens[0], "stall-a", ["--ticks", "600"])
    b = h.start_client(port, tokens[1], "stall-b", ["--stall-at-tick", "150", "--max-ms", "60000"])
    code = a.wait(120, "playing on after the drop")
    check(code == 0, "stall-a ended with code %s:\n%s%s" % (code, a.out(), a.err()))
    check(re.search(r"^dropped slot=\d from=\d+ reason=stalled$", a.out(), re.MULTILINE), "stall-a saw no drop:\n" + a.out())
    check(re.search(r"^done ticks=\d+$", a.out(), re.MULTILINE), "stall-a did not finish:\n" + a.out())
    check("slot" in server.err() and "dropped from tick" in server.err(), "the relay did not log the drop:\n" + server.err())
    b.end()
    server.close_stdin()
    check(server.wait(60, "after stdin closed") == 0, "the relay did not stop cleanly")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True)
    parser.add_argument("--client", required=True)
    parser.add_argument("--replay", default="")
    parser.add_argument("--require-deterministic", action="store_true")
    args = parser.parse_args()

    work = Path(tempfile.mkdtemp(prefix="n2lockstep-"))
    h = Harness(args, work)
    try:
        (work / "layout.json").write_text(json.dumps(LAYOUT), encoding="utf-8")
        tokens_file = work / "tokens.json"
        tokens = make_tokens(SimpleNamespace(server=args.server), tokens_file, 2)
        scenario_refusal(h, tokens_file)
        project = work / "project-deterministic"
        project.mkdir()
        make_project(project, True)
        scenario_session(h, project, tokens, tokens_file)
        scenario_desync(h, project, tokens, tokens_file)
        scenario_stall(h, project, tokens, tokens_file)
        say("n2server_lockstep: all scenarios passed")
        return 0
    except Skip as skipped:
        say("SKIPPED: this build cannot run the Deterministic preset (%s)" % skipped)
        return 0
    except Failure as failure:
        say("FAILED: %s" % failure)
        h.dump()
        return 1
    finally:
        h.end_all()
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
