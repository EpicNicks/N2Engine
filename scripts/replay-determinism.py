#!/usr/bin/env python3
"""Cross-leg comparison of the golden replay hashes (netcode stage C2), and the regeneration of the golden.

Standard library only. The same shape as scripts/physics-determinism.py (stage J7), for the physics-free replay scenes
(engine/src/replay/GoldenReplay.cpp, tests/golden/replays/*.n2replay.json). Subcommands:

  compare     the CI job "golden replay": reads the replay-hashes.json every leg uploaded, checks every expected leg is
              there and complete, compares all legs with each other and with the committed golden
              tests/golden/replay-determinism.json, and prints the first differing scene/tick of every leg that
              differs. Exit 1 on any finding.
  golden      writes a golden file from ONE leg's file (the Linux x86_64 Clang leg). Run it only in a pull request that
              says why the golden changes (docs/testing.html, "The golden replay job").
  self-test   proves the comparison can fail: it feeds synthetic legs that differ, lack a record or are absent, and
              checks each is reported. The CI job runs it first, so a green job cannot come from a comparison that never
              fails.

A mismatch is a result, never something to loosen: this script has no tolerance, no ignore list and no way to skip a leg.
"""

import argparse
import json
import os
import sys

# The replays, in order (tests/golden/replays, engine/src/replay/GoldenReplay.cpp SceneNames). A leg that lacks all the
# records of one of them is incomplete.
SCENES = ["transforms", "lua_sim", "lua_driven"]
TICKS = 600
INTERVAL = 60
FORMAT = 1
ARTIFACT_FILE = "replay-hashes.json"
ARTIFACT_PREFIX = "replay-hashes-"
DEFAULT_GOLDEN = "tests/golden/replay-determinism.json"


def sort_key(key):
    scene, kind, tick = key
    index = SCENES.index(scene) if scene in SCENES else len(SCENES)
    return (index, scene, kind, tick)


def expected_ticks():
    return range(INTERVAL, TICKS + 1, INTERVAL)


def records_to_map(records):
    """{(scene, kind, tick): hash}; a key twice is an error (raised as ValueError)."""
    result = {}
    for record in records:
        try:
            key = (record["scene"], record["kind"], int(record["tick"]))
            value = str(record["hash"])
        except (KeyError, TypeError, ValueError) as error:
            raise ValueError("malformed record %r: %s" % (record, error))
        if key in result:
            raise ValueError("record twice: %s %s tick %d" % key)
        result[key] = value
    return result


def load_file(path):
    """The parsed file, or raises ValueError with the reason."""
    try:
        with open(path, "r", encoding="utf-8") as handle:
            data = json.load(handle)
    except OSError as error:
        raise ValueError("cannot read %s: %s" % (path, error))
    except json.JSONDecodeError as error:
        raise ValueError("%s is not valid JSON: %s" % (path, error))
    if not isinstance(data, dict) or data.get("format") != FORMAT:
        raise ValueError("%s: format is not %d" % (path, FORMAT))
    if not isinstance(data.get("records"), list):
        raise ValueError("%s: no records list" % path)
    return data


def check_complete(name, mapping):
    """Findings for a leg (or golden) that lacks a checkpoint of a replay: every scene at every expected tick."""
    findings = []
    for scene in SCENES:
        missing = [tick for tick in expected_ticks() if (scene, "state", tick) not in mapping]
        if len(missing) == len(list(expected_ticks())):
            findings.append("%s has no record of scene '%s'" % (name, scene))
        elif missing:
            findings.append("%s lacks scene '%s' at tick(s) %s" % (name, scene, ",".join(str(t) for t in missing)))
    return findings


def first_difference(name_a, a, name_b, b):
    """A one-line description of the first key (in scene order) where the two maps differ, or None."""
    for key in sorted(set(a) | set(b), key=sort_key):
        scene, kind, tick = key
        if key not in a:
            return "scene '%s' %s tick %d: %s has no such record (%s has %s)" % (scene, kind, tick, name_a, name_b, b[key])
        if key not in b:
            return "scene '%s' %s tick %d: %s has no such record (%s has %s)" % (scene, kind, tick, name_b, name_a, a[key])
        if a[key] != b[key]:
            return "scene '%s' %s tick %d: %s = %s, %s = %s" % (scene, kind, tick, name_a, a[key], name_b, b[key])
    return None


def count_differences(a, b):
    return sum(1 for key in set(a) | set(b) if a.get(key) != b.get(key))


def compare_legs(legs, golden):
    """legs: {name: map or None (None = the leg's file is missing or unreadable)}, golden: map or None.
    Returns (findings, notes). findings fail the job; notes are information."""
    findings = []
    notes = []
    loaded = {name: mapping for name, mapping in legs.items() if mapping is not None}
    for name, mapping in legs.items():
        if mapping is None:
            findings.append("leg %s: no usable %s (the leg failed, was skipped or uploaded nothing)" % (name, ARTIFACT_FILE))
        else:
            findings.extend(check_complete("leg " + name, mapping))
    names = list(loaded)
    if not names:
        findings.append("no leg produced a usable file: nothing to compare")
        return findings, notes
    reference = names[0]
    for name in names[1:]:
        difference = first_difference(reference, loaded[reference], name, loaded[name])
        if difference:
            findings.append("leg %s differs from leg %s, first at %s (%d records differ)"
                            % (name, reference, difference, count_differences(loaded[reference], loaded[name])))
    if golden is None:
        notes.append("golden missing: %s does not exist, so the legs were compared only with each other. Commit the golden "
                     "from a green run (docs/testing.html, 'The golden replay job')." % DEFAULT_GOLDEN)
    else:
        findings.extend(check_complete("golden", golden))
        for name in names:
            difference = first_difference(name, loaded[name], "golden", golden)
            if difference:
                findings.append("leg %s differs from the golden, first at %s (%d records differ)"
                                % (name, difference, count_differences(loaded[name], golden)))
    if len(names) == 1:
        findings.append("only one leg was compared: a comparison needs at least two")
    return findings, notes


def summarize(out_lines, text):
    print(text)
    out_lines.append(text)


def write_summary(lines):
    path = os.environ.get("GITHUB_STEP_SUMMARY")
    if not path:
        return
    try:
        with open(path, "a", encoding="utf-8") as handle:
            handle.write("\n".join(lines) + "\n")
    except OSError:
        pass


def run_compare(args):
    expected = [leg for leg in args.expect.split(",") if leg]
    if not expected:
        print("--expect names no leg")
        return 1
    lines = ["### Golden replay hashes (%d legs)" % len(expected), ""]
    legs = {}
    fingerprints = {}
    for leg in expected:
        path = os.path.join(args.legs_dir, ARTIFACT_PREFIX + leg, ARTIFACT_FILE)
        try:
            data = load_file(path)
            if data.get("leg") != leg:
                raise ValueError("%s says leg '%s', expected '%s'" % (path, data.get("leg"), leg))
            legs[leg] = records_to_map(data["records"])
            fingerprints[leg] = data.get("fingerprint")
        except ValueError as error:
            print("leg %s: %s" % (leg, error))
            lines.append("- leg `%s`: %s" % (leg, error))
            legs[leg] = None

    golden = None
    golden_error = None
    if os.path.exists(args.golden):
        try:
            golden = records_to_map(load_file(args.golden)["records"])
        except ValueError as error:
            golden_error = str(error)
    if golden_error:
        # A golden that exists but cannot be read must never read as "missing"
        print("golden: " + golden_error)
        lines.append("- golden: " + golden_error)

    findings, notes = compare_legs(legs, golden)
    if golden_error:
        findings.append("golden unreadable: " + golden_error)

    for leg in expected:
        if fingerprints.get(leg):
            summarize(lines, "leg %s fingerprint: %s" % (leg, json.dumps(fingerprints[leg], sort_keys=True)))
    for note in notes:
        summarize(lines, "NOTE: " + note)
    for finding in findings:
        summarize(lines, "MISMATCH: " + finding)

    if golden is None and not golden_error:
        reference = next((leg for leg in expected if legs.get(leg) is not None), None)
        if reference is not None:
            lines.append("")
            lines.append("<details><summary>Hashes of leg %s (what the golden would hold)</summary>" % reference)
            lines.append("")
            lines.append("    scene kind tick hash")
            for key in sorted(legs[reference], key=sort_key):
                lines.append("    %s %s %d %s" % (key[0], key[1], key[2], legs[reference][key]))
            lines.append("")
            lines.append("</details>")

    if findings:
        summarize(lines, "")
        summarize(lines, "RESULT: %d finding(s). A difference between legs is a real result; do not loosen the comparison." % len(findings))
        write_summary(lines)
        return 1
    summarize(lines, "")
    summarize(lines, "RESULT: all %d legs are equal%s." % (len(expected), " and equal to the golden" if golden is not None else " (no golden to compare with yet)"))
    write_summary(lines)
    return 0


def run_golden(args):
    try:
        data = load_file(args.source)
        mapping = records_to_map(data["records"])
    except ValueError as error:
        print(error)
        return 1
    problems = check_complete("source", mapping)
    if problems:
        for problem in problems:
            print(problem)
        return 1
    ordered = sorted(mapping, key=sort_key)
    golden = {
        "format": FORMAT,
        "note": "Generated by scripts/replay-determinism.py golden from the Linux x86_64 Clang leg. Change only in a pull "
                "request that says why (a deliberate change of a scene or a replay file, a toolchain bump that proved safe).",
        "ticks": data.get("ticks"),
        "interval": data.get("interval"),
        "records": [{"scene": key[0], "kind": key[1], "tick": key[2], "hash": mapping[key]} for key in ordered],
    }
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w", encoding="utf-8", newline="\n") as handle:
        json.dump(golden, handle, indent=1)
        handle.write("\n")
    print("wrote %s (%d records) from leg %s" % (args.out, len(ordered), data.get("leg")))
    return 0


def synthetic(offset=0):
    mapping = {}
    for index, scene in enumerate(SCENES):
        for tick in expected_ticks():
            mapping[(scene, "state", tick)] = "0x%016x" % (index * 1000 + tick + offset)
    return mapping


def run_self_test(_args):
    failures = []

    def expect(condition, what):
        if not condition:
            failures.append(what)

    base = synthetic()
    findings, notes = compare_legs({"a": dict(base), "b": dict(base)}, dict(base))
    expect(not findings, "equal legs and golden were reported: %s" % findings)

    findings, notes = compare_legs({"a": dict(base), "b": dict(base)}, None)
    expect(not findings, "equal legs without a golden were reported")
    expect(any("golden missing" in note for note in notes), "a missing golden was not noted")

    changed = dict(base)
    changed[("lua_sim", "state", 240)] = "0xdeadbeef"
    findings, _ = compare_legs({"a": dict(base), "b": changed}, dict(base))
    expect(any("leg b differs from leg a" in f and "scene 'lua_sim' state tick 240" in f for f in findings),
           "a differing record between legs was not reported with its scene and tick: %s" % findings)
    expect(any("leg b differs from the golden" in f for f in findings), "a leg differing from the golden was not reported")

    # Two legs equal to each other but not to the golden
    findings, _ = compare_legs({"a": dict(changed), "b": dict(changed)}, dict(base))
    expect(any("leg a differs from the golden" in f for f in findings), "legs equal to each other but not the golden were accepted")

    short = dict(base)
    del short[("lua_driven", "state", 600)]
    findings, _ = compare_legs({"a": dict(base), "b": short}, None)
    expect(any("lua_driven" in f and "600" in f for f in findings), "a missing record was not reported: %s" % findings)

    findings, _ = compare_legs({"a": dict(base), "b": None}, dict(base))
    expect(any("leg b: no usable" in f for f in findings), "a missing leg was not reported")

    emptied = {key: value for key, value in base.items() if key[0] != "lua_sim"}
    findings, _ = compare_legs({"a": dict(base), "b": emptied}, None)
    expect(any("no record of scene 'lua_sim'" in f for f in findings), "a missing scene was not reported")

    findings, _ = compare_legs({"a": None, "b": None}, None)
    expect(bool(findings), "no usable leg at all was not reported")

    findings, _ = compare_legs({"a": dict(base)}, dict(base))
    expect(any("only one leg" in f for f in findings), "a single leg was accepted as a comparison")

    try:
        records_to_map([{"scene": "a", "kind": "state", "tick": 1, "hash": "0x1"}] * 2)
        expect(False, "a record twice was accepted")
    except ValueError:
        pass

    for failure in failures:
        print("SELF-TEST FAILED: " + failure)
    if failures:
        return 1
    print("self-test: the comparison reports a differing record, a missing record, a missing scene, a missing leg and a golden mismatch")
    return 0


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    compare = sub.add_parser("compare", help="compare the legs' files with each other and with the golden")
    compare.add_argument("--legs-dir", required=True, help="the folder the artifacts were downloaded into (one subfolder per artifact)")
    compare.add_argument("--expect", required=True, help="comma-separated leg names; every one must be present and complete")
    compare.add_argument("--golden", default=DEFAULT_GOLDEN)
    compare.set_defaults(func=run_compare)
    golden = sub.add_parser("golden", help="write the golden file from one leg's replay-hashes.json")
    golden.add_argument("--from", dest="source", required=True)
    golden.add_argument("--out", default=DEFAULT_GOLDEN)
    golden.set_defaults(func=run_golden)
    selftest = sub.add_parser("self-test", help="check that the comparison can fail")
    selftest.set_defaults(func=run_self_test)
    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
