#!/usr/bin/env python3
# editor-server/protocol/tests/run_tests.py
#
# The protocol's generator tests, with no build: Python 3.9+ and Node 22.6+ (for --experimental-strip-types).
#   1. Runs every generator on fixture-protocol.json (every field type, including ones protocol.json doesn't use yet)
#      into a temporary folder, so a generator that breaks on mat4, quat, json shapes or primitive arrays fails here.
#   2. Runs typescript-codecs.test.mjs on the checked-in TypeScript client and golden vectors, then on the fixture's.
# It doesn't check that the checked-in outputs are up to date; CI regenerates them and diffs (see tests.yml).

import os
import subprocess
import sys
import tempfile
from pathlib import Path

TESTS_DIR = Path(__file__).parent
GENERATORS_DIR = TESTS_DIR.parent / "generators"
FIXTURE = TESTS_DIR / "fixture-protocol.json"
NODE_TEST = TESTS_DIR / "typescript-codecs.test.mjs"


def run(command, env=None):
    print("+ " + " ".join(str(part) for part in command), flush=True)
    subprocess.run([str(part) for part in command], check=True, env=env)


def main():
    node = os.environ.get("NODE", "node")
    node_test = [node, "--experimental-strip-types", "--test", NODE_TEST]

    run(node_test)

    with tempfile.TemporaryDirectory() as temp:
        out = Path(temp)
        outputs = {
            "generate_cpp.py": out / "fixture.generated.hpp",
            "generate_csharp.py": out / "fixture.generated.cs",
            "generate_typescript.py": out / "fixture.generated.ts",
            "generate_test_vectors.py": out / "fixture-vectors.json",
        }
        for generator, output in outputs.items():
            run([sys.executable, "-B", GENERATORS_DIR / generator, "--protocol", FIXTURE, "--out", output])

        env = dict(os.environ,
                   N2_PROTOCOL_JSON=str(FIXTURE),
                   N2_PROTOCOL_TS=str(outputs["generate_typescript.py"]),
                   N2_PROTOCOL_VECTORS=str(outputs["generate_test_vectors.py"]))
        run(node_test, env)


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as error:
        sys.exit(error.returncode or 1)
