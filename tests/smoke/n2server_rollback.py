#!/usr/bin/env python3
"""Rollback and late-spectator soak (netcode C5), one script for Windows and Linux (standard library only; ctest runs it as
n2server_rollback).

Real processes on real loopback UDP, through the secure handshake: the real `N2Server --relay` and headless clients
(n2server_rollback_client: a LockstepRuntime in ROLLBACK mode over the C2 replay scene, whose Save and Restore are the
snapshot hooks; or a late spectator that starts from a player's snapshot). The client injects the misprediction: a player
freezes for a few ticks now and then (--hitch-every, --hitch-ms), so the other player runs ticks on a guess, and every guess
is wrong (--mispredict 1), so every guessed tick is rolled back. Scenarios:
  - two rollback players play 900 ticks while a spectator joins late (after checkpoint 210) and takes a player's snapshot:
    both players printed confirmed checkpoint hashes that agree, the spectator's agree with them from its start tick on,
    BOTH players really rolled back (the rollback-stats lines: rollbacks, resimulated ticks, mispredictions and predicted
    ticks all above zero: the test cannot pass by never rolling back), and the C2 golden runner (n2_replay, when given)
    replays a player's recorded confirmed inputs to the very hashes the live rollback client printed;
  - a rollback player is killed mid-session: the relay drops its slot (DropSlot), the other player applies it, keeps
    rolling back and plays to the end, its confirmed hashes going on past the drop;
  - the snapshot source (slot 0) is killed right after a late spectator joined, with a padded snapshot (256 KiB, so the
    transfer takes many parts): wherever in the request or the transfer the source dies, the relay asks the next player,
    the spectator still starts, runs and agrees with the survivor. (Where exactly the kill falls, before the request was
    answered or between two parts, is up to timing; the assertions hold for every case.)

  python tests/smoke/n2server_rollback.py --server build/bin/N2Server --client build/bin/n2server_rollback_client \\
      [--replay build/bin/n2_replay] [--require-deterministic]

The Deterministic preset refuses to start without Jolt in a strict-FP build: without --require-deterministic a server that
refuses for that reason makes the run print SKIPPED and exit 0; with it, that is a failure (the Linux legs that build Jolt
with N2ENGINE_STRICT_FP pass it). Any failed assertion exits with code 1 and a message; every process's stdout and stderr
are printed at the end, and every process is ended on the way out.
"""

import argparse
import json
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parent))
from n2server_lockstep import (  # noqa: E402
    LAYOUT, REPLAY_LINE, Harness, Skip, hashes_of, make_project)
from n2server_net import Failure, check, make_tokens, say, text_of, wait_for, wait_line  # noqa: E402

STATS_LINE = re.compile(r"^rollback-stats (.*)$", re.MULTILINE)
WELCOME_LINE = re.compile(r"^welcome .*slot=(\d+)$", re.MULTILINE)
PLAYER = ["--rollback-window", "8", "--mispredict", "1"]


def stats_of(tool):
    match = STATS_LINE.search(tool.out())
    check(match is not None, "%s printed no rollback-stats line:\n%s" % (tool.name, tool.out()))
    return {key: int(value) for key, value in (field.split("=") for field in match.group(1).split())}


def slot_of(tool):
    match = WELCOME_LINE.search(tool.out())
    check(match is not None, "%s printed no welcome line:\n%s" % (tool.name, tool.out()))
    return int(match.group(1))


def check_rolled_back(tool):
    stats = stats_of(tool)
    for key in ("rollbacks", "resimulated", "mispredictions", "predicted"):
        check(stats[key] > 0, "%s never rolled back (%s = 0): %s" % (tool.name, key, stats))
    say("  %s: %d rollbacks, %d ticks resimulated, deepest %d" % (tool.name, stats["rollbacks"], stats["resimulated"], stats["deepest"]))


def agree(first, second, what, minimum):
    ha, hb = hashes_of(first), hashes_of(second)
    common = sorted(set(ha) & set(hb))
    check(len(common) >= minimum, "%s: %s and %s share only %d checkpoints (need %d)" % (what, first.name, second.name, len(common), minimum))
    for tick in common:
        check(ha[tick] == hb[tick], "%s: the hashes differ at tick %d: %s %s vs %s %s" % (what, tick, first.name, ha[tick], second.name, hb[tick]))
    say("  %s: %d checkpoints agree between %s and %s" % (what, len(common), first.name, second.name))
    return common


def finished_ok(tool, what):
    code = tool.wait(180, what)
    check(code == 0, "%s ended with code %s:\n%s%s" % (tool.name, code, tool.out(), tool.err()))
    check(re.search(r"^done ticks=\d+$", tool.out(), re.MULTILINE), "%s did not finish:\n%s%s" % (tool.name, tool.out(), tool.err()))


def start_relay_end(h, server):
    server.close_stdin()
    check(server.wait(60, "after stdin closed") == 0, "the relay did not stop cleanly")


def scenario_rollback_and_spectator(h, project, tokens, tokens_file):
    say("two rollback players, a late spectator, real rollbacks")
    server, port = h.start_relay(project, tokens_file, "relay-rollback")
    a_out = h.work / "a.n2replay.json"
    b_out = h.work / "b.n2replay.json"
    a = h.start_client(port, tokens[0], "rb-a", PLAYER + ["--ticks", "900", "--hitch-every", "40", "--hitch-ms", "80", "--replay-out", str(a_out)])
    b = h.start_client(port, tokens[1], "rb-b", PLAYER + ["--ticks", "900", "--hitch-every", "55", "--hitch-ms", "60", "--replay-out", str(b_out)])
    wait_line(a, r"^hash tick=210 ", 90, "checkpoint 210 of the players", a, b)
    spectator = h.start_client(port, tokens[2], "rb-spectator", ["--spectate", "1", "--ticks", "600"])
    for client in (a, b, spectator):
        finished_ok(client, "playing")
    check(slot_of(spectator) == 255, "the spectator must be welcomed with slot 255")
    check(sorted([slot_of(a), slot_of(b)]) == [0, 1], "the players must take slots 0 and 1")

    agree(a, b, "players", 20)
    joined = re.search(r"^spectator-joined tick=(\d+) snapshotBytes=(\d+)$", spectator.out(), re.MULTILINE)
    check(joined is not None, "the spectator never took a snapshot:\n" + spectator.out())
    join_tick = int(joined.group(1))
    check(join_tick >= 210, "the spectator joined at tick %d, before it was asked to" % join_tick)
    common = agree(a, spectator, "spectator", 5)
    check(min(common) > join_tick - 1, "the spectator reported a checkpoint before its start tick")
    check(not re.search(r"^dropped |^stopped ", spectator.out(), re.MULTILINE), "the spectator was stopped:\n" + spectator.out())
    check(re.search(r"^snapshot-sent tick=\d+$", a.out() + b.out(), re.MULTILINE), "no player reported sending its snapshot")

    check_rolled_back(a)
    check_rolled_back(b)

    # The recorded confirmed inputs of the two players are the same stream
    rows = []
    for path in (a_out, b_out):
        log = json.loads(path.read_text(encoding="utf-8"))
        rows.append((log["ticks"], log["inputs"]))
    shared = min(rows[0][0], rows[1][0])
    check(shared >= 600, "a recorded log is only %d ticks long" % shared)
    check([r for r in rows[0][1] if r[0] < shared] == [r for r in rows[1][1] if r[0] < shared],
          "the two players' recorded input logs differ")

    if h.args.replay and Path(h.args.replay).exists():
        result = subprocess.run([str(h.args.replay), str(a_out)], capture_output=True, timeout=300)
        check(result.returncode == 0, "n2_replay failed on the recording:\n" + text_of(result.stdout) + text_of(result.stderr))
        ha = hashes_of(a)
        replayed = {int(t): v for t, v in REPLAY_LINE.findall(text_of(result.stdout))}
        compared = 0
        for tick, value in replayed.items():
            if tick in ha:
                check(ha[tick] == value, "the replay's hash at tick %d (%s) is not the rollback client's (%s)" % (tick, value, ha[tick]))
                compared += 1
        check(compared >= 20, "only %d replayed checkpoints could be compared" % compared)
        say("  the C2 golden runner replays the rollback player's recording to its live hashes (%d checkpoints)" % compared)
    else:
        say("  (no n2_replay given: the replay of the recording was not run)")

    wait_for(lambda: "the session is over; waiting for the next one" in server.err(), 30, "the relay's session end", server)
    start_relay_end(h, server)


def scenario_player_killed(h, project, tokens, tokens_file):
    say("a rollback player is killed mid-session; the other applies the drop and plays on")
    server, port = h.start_relay(project, tokens_file, "relay-drop", ["--relay-stall-ms", "1500"])
    a = h.start_client(port, tokens[0], "drop-a", PLAYER + ["--ticks", "800", "--hitch-every", "40", "--hitch-ms", "80"])
    b = h.start_client(port, tokens[1], "drop-b", PLAYER + ["--ticks", "800", "--hitch-every", "55", "--hitch-ms", "60"])
    wait_line(a, r"^hash tick=150 ", 90, "checkpoint 150", a, b)
    before = max(hashes_of(a))
    b.end()
    finished_ok(a, "playing on after the drop")
    dropped = re.search(r"^dropped slot=(\d) from=(\d+) reason=(\S+)$", a.out(), re.MULTILINE)
    check(dropped is not None, "drop-a saw no drop:\n" + a.out())
    check(int(dropped.group(1)) == slot_of(b), "the dropped slot is not the killed player's")
    after = max(hashes_of(a))
    check(after >= before + 300, "the survivor's confirmed hashes did not go on past the drop (%d -> %d)" % (before, after))
    check(re.search(r"dropped from tick", server.err()), "the relay did not log the drop:\n" + server.err())
    check_rolled_back(a)
    start_relay_end(h, server)


def scenario_source_killed(h, project, tokens, tokens_file):
    say("the snapshot source is killed right after a late spectator joined; the next player serves it")
    server, port = h.start_relay(project, tokens_file, "relay-source", ["--relay-stall-ms", "1500"])
    pad = ["--snapshot-pad-kb", "256"]
    a = h.start_client(port, tokens[0], "src-a", PLAYER + pad + ["--ticks", "900", "--hitch-every", "45", "--hitch-ms", "70"])
    b = h.start_client(port, tokens[1], "src-b", PLAYER + pad + ["--ticks", "900", "--hitch-every", "60", "--hitch-ms", "60"])
    wait_line(a, r"^hash tick=150 ", 90, "checkpoint 150", a, b)
    spectator = h.start_client(port, tokens[2], "src-spectator", ["--spectate", "1", "--ticks", "600"])
    wait_line(spectator, r"^welcome ", 30, "the spectator's welcome", spectator)
    # The relay asks the lowest live slot for the snapshot: that process dies now
    source, survivor = (a, b) if slot_of(a) == 0 else (b, a)
    check(slot_of(source) == 0, "no player has slot 0")
    source.end()
    finished_ok(survivor, "playing on after the source died")
    finished_ok(spectator, "running from the snapshot")
    check(re.search(r"^spectator-joined tick=\d+ snapshotBytes=\d+$", spectator.out(), re.MULTILINE),
          "the spectator never took a snapshot:\n" + spectator.out())
    check(re.search(r"^dropped slot=0 ", survivor.out(), re.MULTILINE), "the survivor saw no drop of slot 0:\n" + survivor.out())
    agree(survivor, spectator, "spectator after the source died", 5)
    check(not re.search(r"^dropped |^stopped ", spectator.out(), re.MULTILINE), "the spectator was stopped:\n" + spectator.out())
    check_rolled_back(survivor)
    start_relay_end(h, server)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True)
    parser.add_argument("--client", required=True)
    parser.add_argument("--replay", default="")
    parser.add_argument("--require-deterministic", action="store_true")
    args = parser.parse_args()

    work = Path(tempfile.mkdtemp(prefix="n2rollback-"))
    h = Harness(args, work)
    try:
        (work / "layout.json").write_text(json.dumps(LAYOUT), encoding="utf-8")
        tokens_file = work / "tokens.json"
        tokens = make_tokens(SimpleNamespace(server=args.server), tokens_file, 3)
        project = work / "project-deterministic"
        project.mkdir()
        make_project(project, True)
        scenario_rollback_and_spectator(h, project, tokens, tokens_file)
        scenario_player_killed(h, project, tokens, tokens_file)
        scenario_source_killed(h, project, tokens, tokens_file)
        say("n2server_rollback: all scenarios passed")
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
