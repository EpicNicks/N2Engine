"""Writes the test packs in this folder (tests/engine/io/VfsPackTests.cpp reads them):

  sample.zip        deflate entries, one stored entry, a folder entry
  sample.7z         the same files, non-solid (7z -ms=off: one block per file), LZMA2
  sample-solid.7z   the same files in one solid block

Every pack holds the same files with the same bytes (FILES below), so the tests compare each mount against FILES.
Needs Python 3 and 7-Zip's command line (7z or 7za on PATH, or 7Z set to its path). Run it from anywhere:

  python tests/assets/packs/make_packs.py
"""
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))

FILES = {
    "project.n2proj": b'{"formatVersion":1,"name":"Pack Sample"}\n',
    "assets/scripts/main.lua": b"-- a script\nreturn { answer = 42 }\n" * 20,
    "assets/sub/notes.txt": "ünïcödé text\n".encode("utf-8"),
    "assets/data.bin": bytes(range(256)) * 8,
    "assets/empty.txt": b"",
}
STORED = {"assets/data.bin"}


def find_7z():
    for candidate in (os.environ.get("7Z"), "7z", "7za", r"C:\Program Files\7-Zip\7z.exe"):
        if candidate and (shutil.which(candidate) or os.path.isfile(candidate)):
            return shutil.which(candidate) or candidate
    sys.exit("7-Zip's command line was not found (set 7Z to it)")


def main():
    # A fixed time, so a rerun writes the same zip
    stamp = (2026, 11, 11, 12, 0, 0)
    with zipfile.ZipFile(os.path.join(HERE, "sample.zip"), "w") as z:
        z.writestr(zipfile.ZipInfo("assets/", stamp), b"")
        for name, data in FILES.items():
            info = zipfile.ZipInfo(name, stamp)
            info.compress_type = zipfile.ZIP_STORED if name in STORED else zipfile.ZIP_DEFLATED
            z.writestr(info, data)

    seven = find_7z()
    with tempfile.TemporaryDirectory() as staging:
        for name, data in FILES.items():
            path = os.path.join(staging, *name.split("/"))
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as f:
                f.write(data)
        for output, solid in (("sample.7z", "off"), ("sample-solid.7z", "on")):
            target = os.path.join(HERE, output)
            if os.path.exists(target):
                os.remove(target)
            subprocess.run([seven, "a", "-t7z", "-m0=LZMA2", f"-ms={solid}", "-mx=5", target, "*"],
                           cwd=staging, check=True, stdout=subprocess.DEVNULL)
    print("wrote sample.zip, sample.7z, sample-solid.7z")


if __name__ == "__main__":
    main()
