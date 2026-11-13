#!/usr/bin/env python3
"""MoltenVK's size and speed on macOS and iOS (build targets stage 13; orchestrator/proposals/metal-spike.md, docs/apple.html).

The measurement workflow (.github/workflows/apple-measure.yml) runs it on macOS runners. Standard library only.
Subcommands, each writing one JSON record (--out) that `report` turns into the tables of the run's summary:

  size     a package or app: every file's bytes summed, the executable as built, the executable with its debug and local
           symbols stripped (a copy; strip -S -x), the package as one deflate zip (level 9: about what a download costs),
           and the top-level files' sizes. --extra NAME=PATH adds a file's size (MoltenVK's static libraries).
  frames   the time per frame of a command that takes --frames N: it runs the command for --short and for --long frames
           and divides the difference in wall time by the difference in frames, so start-up (the window, the device,
           the pipelines, the game's scripts) cancels out. Repeated --repeats times; the median is reported. Every run
           must exit 0 and print --expect (the renderer the player says it uses).
  fps      the frame-rate lines a player logs ("<frames> frames, <fps> fps" every five seconds, the iOS player's), from a
           log file. The first line (start-up) is left out; at least three must remain.
  report   reads every record in a folder and prints Markdown: the sizes, what MoltenVK adds, the frame times.

Numbers from GitHub's macOS runners are numbers of a virtual machine (Apple's paravirtual GPU) and of the iOS simulator,
whose Metal runs on that GPU through the Mac. They are not a Mac's or an iPhone's.
"""

import argparse
import json
import os
import re
import statistics
import subprocess
import sys
import tempfile
import time
import zipfile


def write_record(path, record):
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(record, f, indent=2)
        f.write("\n")
    print(json.dumps(record, indent=2))


def tree_bytes(path):
    """Bytes of every regular file under path (or of path itself), symbolic links not followed"""
    if os.path.isfile(path):
        return os.lstat(path).st_size
    total = 0
    for root, _dirs, files in os.walk(path):
        for name in files:
            full = os.path.join(root, name)
            if not os.path.islink(full):
                total += os.lstat(full).st_size
    return total


def zip_bytes(path):
    """The package as one zip, deflate level 9, in name order"""
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, "package.zip")
        base = os.path.dirname(os.path.abspath(path))
        with zipfile.ZipFile(out, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as z:
            if os.path.isfile(path):
                z.write(path, os.path.relpath(path, base))
            else:
                for root, dirs, files in os.walk(path):
                    dirs.sort()
                    for name in sorted(files):
                        full = os.path.join(root, name)
                        if not os.path.islink(full):
                            z.write(full, os.path.relpath(full, base))
        return os.path.getsize(out)


def stripped_bytes(binary):
    """The executable with debug and local symbols stripped, measured on a copy (the package is left as built)"""
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, "stripped")
        subprocess.run(["xcrun", "strip", "-S", "-x", "-o", out, binary], check=True)
        return os.path.getsize(out)


def cmd_size(args):
    if not os.path.exists(args.package):
        sys.exit(f"{args.package} does not exist")
    if not os.path.isfile(args.binary):
        sys.exit(f"{args.binary} is not a file")
    top = {}
    for name in sorted(os.listdir(args.package)):
        full = os.path.join(args.package, name)
        if not os.path.islink(full):
            top[name] = tree_bytes(full)
    extras = {}
    for item in args.extra or []:
        name, _, path = item.partition("=")
        if not name or not path or not os.path.exists(path):
            sys.exit(f"--extra {item}: NAME=PATH to an existing file")
        extras[name] = tree_bytes(path)
    write_record(args.out, {
        "kind": "size",
        "id": args.id,
        "label": args.label,
        "packageBytes": tree_bytes(args.package),
        "zipBytes": zip_bytes(args.package),
        "binaryBytes": os.path.getsize(args.binary),
        "binaryStrippedBytes": stripped_bytes(args.binary),
        "files": top,
        "extras": extras,
    })


def run_frames(command, frames, expect):
    started = time.perf_counter()
    done = subprocess.run(command + ["--frames", str(frames)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          timeout=900, check=False)
    seconds = time.perf_counter() - started
    output = done.stdout.decode("utf-8", errors="replace")
    if done.returncode != 0:
        sys.exit(f"{' '.join(command)} --frames {frames} exited {done.returncode}:\n{output[-4000:]}")
    if expect and expect not in output:
        sys.exit(f"{' '.join(command)} --frames {frames} did not print '{expect}':\n{output[-4000:]}")
    return seconds, output


def cmd_frames(args):
    command = args.command
    if command and command[0] == "--":
        command = command[1:]
    if not command:
        sys.exit("frames: the command goes after --")
    if args.long <= args.short or args.short < 1:
        sys.exit("frames: --long must be more than --short, and --short at least 1")
    per_frame_ms = []
    short_seconds = []
    last_output = ""
    for _ in range(args.repeats):
        short, _out = run_frames(command, args.short, args.expect)
        long, last_output = run_frames(command, args.long, args.expect)
        short_seconds.append(short)
        per_frame_ms.append((long - short) * 1000.0 / (args.long - args.short))
    if args.log:
        with open(args.log, "w", encoding="utf-8") as f:
            f.write(last_output)
    median = statistics.median(per_frame_ms)
    if median <= 0:
        sys.exit(f"frames: the longer run was not slower ({per_frame_ms}): too few frames to measure")
    write_record(args.out, {
        "kind": "frames",
        "id": args.id,
        "label": args.label,
        "short": args.short,
        "long": args.long,
        "repeats": args.repeats,
        "perFrameMs": [round(v, 3) for v in per_frame_ms],
        "medianMs": round(median, 3),
        "fps": round(1000.0 / median, 1),
        "startupAndShortMedianS": round(statistics.median(short_seconds), 2),
    })


FPS_LINE = re.compile(r"(\d+) frames, ([0-9.]+) fps")


def cmd_fps(args):
    with open(args.log, encoding="utf-8", errors="replace") as f:
        text = f.read()
    if args.expect and args.expect not in text:
        sys.exit(f"fps: {args.log} does not hold '{args.expect}'")
    values = [float(m.group(2)) for m in FPS_LINE.finditer(text)]
    if len(values) < 4:
        sys.exit(f"fps: {args.log} has {len(values)} frame-rate lines; at least 4 are needed (the first is left out)")
    kept = values[1:]
    write_record(args.out, {
        "kind": "fps",
        "id": args.id,
        "label": args.label,
        "lines": len(kept),
        "medianFps": round(statistics.median(kept), 1),
        "minFps": min(kept),
        "maxFps": max(kept),
        "medianMs": round(1000.0 / statistics.median(kept), 1),
    })


def mib(n):
    return f"{n / (1024 * 1024):,.2f} MiB"


def signed_mib(n):
    return ("+" if n >= 0 else "-") + mib(abs(n))


def cmd_report(args):
    records = []
    for root, _dirs, files in os.walk(args.folder):
        for name in sorted(files):
            if name.endswith(".json"):
                with open(os.path.join(root, name), encoding="utf-8") as f:
                    records.append(json.load(f))
    sizes = {r["id"]: r for r in records if r.get("kind") == "size"}
    timings = [r for r in records if r.get("kind") in ("frames", "fps")]
    out = ["### MoltenVK on macOS and iOS: size and speed", "",
           "GitHub's macOS runners are virtual machines (Apple's paravirtual GPU), and the iOS simulator's Metal runs on that "
           "GPU through the Mac: these are not a Mac's or an iPhone's numbers. Release builds of lua_project.", ""]
    if sizes:
        out += ["| Build | Package | Package, zipped | Executable | Executable, stripped |", "|---|---|---|---|---|"]
        for key in sorted(sizes):
            r = sizes[key]
            out.append(f"| {r['label']} | {mib(r['packageBytes'])} | {mib(r['zipBytes'])} | {mib(r['binaryBytes'])} | "
                       f"{mib(r['binaryStrippedBytes'])} |")
        out.append("")
        lines = []
        mac_with, mac_without = sizes.get("macos-moltenvk"), sizes.get("macos-no-moltenvk")
        if mac_with and mac_without:
            dylib = mac_with["files"].get("libMoltenVK.dylib", 0)
            renderer = mac_with["binaryStrippedBytes"] - mac_without["binaryStrippedBytes"]
            lines.append(f"- macOS: MoltenVK itself (`libMoltenVK.dylib`, beside the player) {mib(dylib)}; the player's "
                         f"Vulkan renderer {signed_mib(renderer)} (stripped executables); the package "
                         f"{signed_mib(mac_with['packageBytes'] - mac_without['packageBytes'])}, zipped "
                         f"{signed_mib(mac_with['zipBytes'] - mac_without['zipBytes'])}.")
        ios_with, ios_without = sizes.get("ios-moltenvk"), sizes.get("ios-no-moltenvk")
        if ios_with and ios_without:
            delta = ios_with["binaryStrippedBytes"] - ios_without["binaryStrippedBytes"]
            line = (f"- iOS (simulator app): MoltenVK and the Vulkan renderer, linked into the executable, "
                    f"{signed_mib(delta)} (stripped); the app {signed_mib(ios_with['packageBytes'] - ios_without['packageBytes'])}, "
                    f"zipped {signed_mib(ios_with['zipBytes'] - ios_without['zipBytes'])}.")
            if mac_with and mac_without:
                renderer = mac_with["binaryStrippedBytes"] - mac_without["binaryStrippedBytes"]
                line += (f" Less the Vulkan renderer's share measured on macOS, MoltenVK's own part is about "
                         f"{mib(delta - renderer)}.")
            lines.append(line)
            for name, value in sorted(ios_with.get("extras", {}).items()):
                lines.append(f"- {name}: {mib(value)} (the static library before linking; the linker keeps only what is used)")
        out += lines + [""]
    if timings:
        out += ["| Run | Frame time | Frame rate | How |", "|---|---|---|---|"]
        for r in sorted(timings, key=lambda r: r["id"]):
            if r["kind"] == "frames":
                how = (f"median of {r['repeats']}: ({r['long']} frames - {r['short']} frames) wall time / {r['long'] - r['short']}; "
                       f"each repeat {', '.join(f'{v:.2f}' for v in r['perFrameMs'])} ms")
                out.append(f"| {r['label']} | {r['medianMs']:.2f} ms | {r['fps']:.1f} fps | {how} |")
            else:
                how = f"median of {r['lines']} five-second lines (first left out), {r['minFps']} to {r['maxFps']} fps"
                out.append(f"| {r['label']} | {r['medianMs']:.1f} ms | {r['medianFps']:.1f} fps | {how} |")
        out.append("")
    if not sizes and not timings:
        out.append("No records were found: see the jobs' logs.")
    print("\n".join(out))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("size")
    p.add_argument("--id", required=True)
    p.add_argument("--label", required=True)
    p.add_argument("--package", required=True, help="the package folder or the .app")
    p.add_argument("--binary", required=True, help="the executable inside it")
    p.add_argument("--extra", action="append", help="NAME=PATH: another file's size, recorded beside")
    p.add_argument("--out", required=True)
    p.set_defaults(func=cmd_size)

    p = sub.add_parser("frames")
    p.add_argument("--id", required=True)
    p.add_argument("--label", required=True)
    p.add_argument("--short", type=int, default=30)
    p.add_argument("--long", type=int, default=330)
    p.add_argument("--repeats", type=int, default=3)
    p.add_argument("--expect", default="")
    p.add_argument("--log", default="")
    p.add_argument("--out", required=True)
    p.add_argument("command", nargs=argparse.REMAINDER)
    p.set_defaults(func=cmd_frames)

    p = sub.add_parser("fps")
    p.add_argument("--id", required=True)
    p.add_argument("--label", required=True)
    p.add_argument("--log", required=True)
    p.add_argument("--expect", default="")
    p.add_argument("--out", required=True)
    p.set_defaults(func=cmd_fps)

    p = sub.add_parser("report")
    p.add_argument("folder")
    p.set_defaults(func=cmd_report)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
