#!/usr/bin/env python3
"""Turns the "[BENCH]" lines of tests/engine/physics/PhysicsBackendBenchTests.cpp into Markdown tables (issue #381 item 7).

Usage: physics-bench-summary.py --leg NAME LOG [LOG ...]

Reads every log, keeps the lines that start with "[BENCH]" (key=value pairs), prints the tables and appends them to the
file $GITHUB_STEP_SUMMARY names, when it is set. Timings from shared CI runners are noise: the tables are for reading and
comparing legs, nothing here judges a number. Exit 1 only when no "[BENCH]" line was found (the bench did not run).
Standard library only.
"""

import argparse
import os
import sys


def parse(line):
    """{key: value} of one "[BENCH] k=v k=v" line, or None."""
    text = line.strip()
    marker = text.find("[BENCH]")
    if marker < 0:
        return None
    fields = {}
    for part in text[marker + len("[BENCH]"):].split():
        if "=" in part:
            key, value = part.split("=", 1)
            fields[key] = value
    return fields if "kind" in fields else None


def table(rows, columns):
    lines = ["| " + " | ".join(title for title, _ in columns) + " |",
             "|" + "|".join("---:" if index > 1 else "---" for index in range(len(columns))) + "|"]
    for row in rows:
        lines.append("| " + " | ".join(row.get(key, "") for _, key in columns) + " |")
    return lines


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--leg", required=True)
    parser.add_argument("logs", nargs="+")
    args = parser.parse_args(argv)

    rows = []
    for path in args.logs:
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as handle:
                for line in handle:
                    fields = parse(line)
                    if fields is not None:
                        rows.append(fields)
        except OSError as error:
            print("cannot read %s: %s" % (path, error))

    out = ["### Physics bench (%s)" % args.leg, "",
           "One run on a shared runner, both backends single-threaded (physics.deterministic). Microseconds; "
           "read as an order of magnitude, compare legs and backends, not runs.", ""]
    steps = [row for row in rows if row.get("kind") == "step"]
    snapshots = [row for row in rows if row.get("kind") == "snapshot"]
    rays = [row for row in rows if row.get("kind") == "raycast"]
    if steps:
        out.append("**Step** (median and worst of 300 ticks after 120 warm-up ticks)")
        out.append("")
        out += table(steps, [("backend", "backend"), ("scene", "scene"), ("bodies", "bodies"), ("joints", "joints"),
                             ("median", "median_us"), ("worst", "worst_us"), ("skipped", "skipped")])
        out.append("")
    if snapshots:
        out.append("**Jolt snapshot** (medians of 200 repeats, after the step measurement)")
        out.append("")
        out += table(snapshots, [("backend", "backend"), ("scene", "scene"), ("bodies", "bodies"), ("joints", "joints"),
                                 ("bytes", "bytes"), ("SaveState", "save_median_us"), ("RestoreState", "restore_median_us"),
                                 ("HashState", "hash_median_us")])
        out.append("")
    if rays:
        out.append("**Raycasts** (one batch, median of 5 batches)")
        out.append("")
        out += table(rays, [("backend", "backend"), ("scene", "scene"), ("bodies", "bodies"), ("rays", "rays"),
                            ("hits", "hits"), ("median", "median_us")])
        out.append("")
    if not rows:
        out.append("No [BENCH] line was found: the bench did not run.")

    print("\n".join(out))
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        try:
            with open(summary, "a", encoding="utf-8") as handle:
                handle.write("\n".join(out) + "\n")
        except OSError as error:
            print("cannot write the step summary: %s" % error)
    return 0 if rows else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
