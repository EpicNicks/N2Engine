#!/usr/bin/env python3
"""Editor host smoke tests, one script for Windows and Linux (standard library only).

Runs the real N2EditorHost end to end. Scenarios:
  host  create a project, start the host on it and talk to it over its TCP protocol, then the exit flags
  tls   start a --tls host, check its identity files, pin its certificate, restart it and require the same one
  all   both (the default)

  python tests/smoke/editor_host_smoke.py --exe build/bin/Debug/N2EditorHost.exe --config Debug
  python3 tests/smoke/editor_host_smoke.py --exe build/bin/N2EditorHost

Any failed assertion exits with code 1 and a message; the hosts' stdout and stderr are printed at the end. See
docs/testing.html (Editor host smoke test).
"""

import argparse
import hashlib
import json
import os
import re
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
import time
import uuid
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import n2_frames as f  # noqa: E402
from n2_frames import SmokeFailure, check  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parents[2]
CREATED_LINE = re.compile(r"^N2EditorHost created (?:.* )?projectId=([0-9a-f-]{36})(?: |$)")
# The launcher regex from logging-and-editor.html#ready-line: port needn't be first
READY_LINE = re.compile(r"^N2EditorHost ready (?:.* )?port=(\d+)(?: |$)")
TLS_READY_LINE = re.compile(r"^N2EditorHost ready port=(\d+) tls=1 fingerprint=([0-9a-f]{64})$")
ZERO_UUID = "00000000-0000-0000-0000-000000000000"


def say(text):
    print(text, flush=True)


class Hosts:
    """Starts host processes with stdout and stderr redirected to files in the work folder, and kills what is left."""

    def __init__(self, exe, work):
        self.exe = str(exe)
        self.work = work
        self.procs = []
        self.logs = []

    def start(self, name, args, env=None, stdin=subprocess.DEVNULL):
        out_path = self.work / (name + ".out")
        err_path = self.work / (name + ".err")
        self.logs.append((name, out_path, err_path))
        with open(out_path, "wb") as out, open(err_path, "wb") as err:
            proc = subprocess.Popen([self.exe] + args, stdin=stdin, stdout=out, stderr=err, env=env)
        proc.n2_out = out_path
        proc.n2_err = err_path
        self.procs.append(proc)
        return proc

    def run(self, args, timeout=60):
        """Runs the host to completion (--create and the like)."""
        return subprocess.run([self.exe] + args, stdin=subprocess.DEVNULL, capture_output=True, timeout=timeout)

    def kill_all(self):
        # A failing kill (the host exiting meanwhile) must not replace the error that got us here
        for proc in self.procs:
            try:
                if proc.poll() is None:
                    proc.kill()
                    proc.wait(timeout=10)
            except Exception as error:  # noqa: BLE001
                say("Couldn't kill host process %s: %s" % (proc.pid, error))

    def dump(self):
        for name, out_path, err_path in self.logs:
            for label, path in (("stdout", out_path), ("stderr", err_path)):
                say("----- host (%s) %s -----" % (name, label))
                try:
                    say(path.read_bytes().decode("utf-8", "replace").rstrip("\n"))
                except OSError:
                    pass


def read_text(path):
    return Path(path).read_bytes().decode("utf-8", "replace")


def wait_exit(proc, seconds, what):
    try:
        return proc.wait(timeout=seconds)
    except subprocess.TimeoutExpired:
        raise SmokeFailure("%s did not exit within %ds" % (what, seconds))


def check_exit_zero(proc, seconds, what):
    code = wait_exit(proc, seconds, what)
    check(code == 0, "%s exited with code 0x%08X" % (what, code & 0xFFFFFFFF))


def find_ready(proc, pattern):
    """The match of the ready line in the host's stdout file, or None. Only complete lines count, each with its
    trailing CR stripped (the host's stdout is in text mode, so on Windows lines end in CR LF)."""
    text = read_text(proc.n2_out)
    end = text.rfind("\n")
    if end < 0:
        return None
    for line in text[:end + 1].split("\n"):
        match = pattern.match(line.rstrip("\r"))
        if match:
            return match
    return None


def wait_ready_line(proc, pattern=READY_LINE, what="Editor host"):
    """Waits (every 500 ms, up to 240 times) for the ready line, as a launcher does, and returns its match."""
    for _ in range(240):
        match = find_ready(proc, pattern)
        if match:
            port = int(match.group(1))
            check(1 <= port <= 65535, "Ready line has an invalid port: %d" % port)
            say("%s ready on port %d" % (what, port))
            return match
        if proc.poll() is not None:
            raise SmokeFailure("%s exited (code %s) before its ready line" % (what, proc.returncode))
        time.sleep(0.5)
    tail = read_text(proc.n2_out)[-2000:]
    raise SmokeFailure("%s never printed its ready line; stdout ends with: %r" % (what, tail))


def user_data_base():
    """The user data folder the host uses (ProjectFile::UserDataBase), worked out from the environment the host
    inherits: %APPDATA%/N2Engine on Windows; elsewhere $XDG_DATA_HOME/n2engine, else $HOME/.local/share/n2engine (an
    empty or relative value counts as unset)."""
    if os.name == "nt":
        appdata = os.environ.get("APPDATA")
        check(appdata, "APPDATA isn't set")
        return Path(appdata) / "N2Engine"
    for name, suffix in (("XDG_DATA_HOME", "n2engine"), ("HOME", ".local/share/n2engine")):
        value = os.environ.get(name, "")
        if value and os.path.isabs(value):
            return Path(value) / suffix
    raise SmokeFailure("Neither XDG_DATA_HOME nor HOME is an absolute path: the host has no user data folder")


def same_path(a, b):
    return os.path.normcase(os.path.normpath(str(a))) == os.path.normcase(os.path.normpath(str(b)))


def protocol_version():
    path = REPO_ROOT / "editor-server" / "protocol" / "protocol.json"
    return json.loads(path.read_text(encoding="utf-8"))["version"]


# ---- Scenario: host ----

def scene_info(conn, frame_type, payload, what):
    """SceneInfo (0x0D): path, name, uuid (strings), revision, savedRevision (uint32)."""
    reader = f.Reader(conn.expect(frame_type, payload, f.SCENE_INFO, what))
    info = {"path": reader.string(), "name": reader.string(), "uuid": reader.string()}
    info["revision"] = reader.u32()
    info["saved"] = reader.u32()
    say("%s: '%s' (%s, %s), revision %d, saved %d" % (
        what, info["path"], info["name"], info["uuid"], info["revision"], info["saved"]))
    return info


def subsystem_states(payload):
    """EngineHealth (0x09): healthy, then a uint32 count of (name, state, detail) strings. As a name -> state map."""
    reader = f.Reader(payload, 1)
    states = {}
    for _ in range(reader.u32()):
        name, state, detail = reader.string(), reader.string(), reader.string()
        states[name] = state
        say("  %s: %s (%s)" % (name, state, detail))
    return states


def get_hierarchy(conn):
    """GetHierarchy (0x28) answers Hierarchy (0x0F): the revision (uint32), then the nodes as json."""
    response = conn.expect(f.GET_HIERARCHY, b"", f.HIERARCHY, "GetHierarchy")
    nodes = json.loads(f.Reader(response, 4).string())
    return {"revision": struct.unpack_from("<I", response, 0)[0], "nodes": nodes, "names": [n["name"] for n in nodes]}


def new_entity(conn, name, parent_id, sibling_index, preset):
    """CreateEntityEx (0x35): name, parentId (strings), siblingIndex (int32), preset (string); EntityCreated: the id."""
    payload = f.wire_string(name) + f.wire_string(parent_id) + f.i32(sibling_index) + f.wire_string(preset)
    return f.Reader(conn.expect(f.CREATE_ENTITY_EX, payload, f.ENTITY_CREATED, "CreateEntityEx " + preset)).string()


def assert_meta(project, name):
    """The asset's .meta: its UUID must be a real one (all zeros means ResourceUUID wasn't initialised)."""
    meta = project / ".import" / (name + ".meta")
    check(meta.exists(), "No metadata for %s at %s" % (name, meta))
    value = json.loads(read_text(meta)).get("uuid")
    check(value and value != ZERO_UUID, "%s has no asset UUID: %r" % (name, value))
    say("%s.meta: uuid %s" % (name, value))


def scenario_create(hosts, work):
    """--create: CPU only (no engine, so no renderer or audio lines), exactly one line on stdout, exit code 0; a
    second --create on the same folder changes nothing and exits with 2. The folder name has a space, as a user's
    would."""
    project = work / "n2 smoke project"
    shutil.rmtree(project, ignore_errors=True)
    result = hosts.run(["--create", str(project), "--name", "Smoke Project"])
    lines = result.stdout.decode("utf-8", "replace").splitlines()
    say("--create (exit %d):" % result.returncode)
    for line in lines:
        say("  " + line)
    for line in result.stderr.decode("utf-8", "replace").splitlines():
        say("  stderr: " + line)
    check(result.returncode == 0, "--create exited with %d" % result.returncode)
    check(len(lines) == 1 and CREATED_LINE.match(lines[0]), "--create didn't print exactly its created line")
    project_id = CREATED_LINE.match(lines[0]).group(1)
    check(re.search(r" startupScene=res://scenes/Main\.scene(?: |$)", lines[0]), "--create: no startup scene in its line")
    project_file = json.loads(read_text(project / "project.n2proj"))
    check(project_file.get("projectId") == project_id and project_file.get("name") == "Smoke Project",
          "project.n2proj doesn't match the created line")
    check((project / "assets" / "scenes" / "Main.scene").exists(), "--create made no Main.scene")
    before = (project / "project.n2proj").read_bytes()
    again = hosts.run(["--create", str(project)])
    check(again.returncode == 2, "--create on a project exited with %d, expected 2" % again.returncode)
    check((project / "project.n2proj").read_bytes() == before, "--create on a project changed project.n2proj")
    say("--create: project %s; again: exit 2, unchanged" % project_id)

    # --project on a folder that isn't a project fails before the engine starts (exit 1); one an older host opened
    # (it has .import/) is told how to adopt it keeping its asset UUIDs
    plain = work / "n2-not-a-project"
    shutil.rmtree(plain, ignore_errors=True)
    (plain / ".import").mkdir(parents=True)
    not_project = hosts.run(["--project", str(plain), "--port", "0"])
    err = not_project.stderr.decode("utf-8", "replace")
    say("--project on a plain folder (exit %d): %s" % (not_project.returncode, err))
    check(not_project.returncode == 1,
          "--project on a folder without project.n2proj exited with %d, expected 1" % not_project.returncode)
    check("--project-id from-path" in err, "--project on a folder with .import/ didn't suggest --create --project-id from-path")
    check("N2EditorHost ready" not in not_project.stdout.decode("utf-8", "replace"),
          "--project on a plain folder printed a ready line")

    # One more asset: the host's ResourceLoader writes its .meta under .import/ at startup
    (project / "assets" / "startup.mat").write_text("{}")
    return project, project_id


def scenario_host(hosts, work, config, version):
    project, project_id = scenario_create(hosts, work)

    # ---- Host 1: --renderer software, --token-env, --project ----
    # The token is only in the environment the host inherits, never on its command line
    token = uuid.uuid4().hex
    env = dict(os.environ, N2_SMOKE_TOKEN=token)
    proc = hosts.start("host", ["--port", "0", "--project", str(project), "--renderer", "software",
                                "--token-env", "N2_SMOKE_TOKEN"], env=env)
    port = int(wait_ready_line(proc).group(1))

    # The host has an access token: a wrong one is refused, and the connection closed
    conn = f.connect(port)
    conn.send(f.HELLO, f.hello_payload("CI smoke test", version, "not the token"))
    response_type, payload = conn.read_response()
    f.assert_response(response_type, payload, f.ERROR, "Hello with a wrong token")
    check(conn.closed_by_host(), "The host kept a connection open after refusing its Hello")
    conn.close()
    say("Hello with a wrong token: refused, connection closed")

    conn = f.connect(port)
    payload = conn.expect(f.HELLO, f.hello_payload("CI smoke test", version, token), f.SERVER_INFO, "Hello with the token")
    server_version = f.Reader(payload).string()
    check(server_version == version, "Hello: the host speaks protocol %s, protocol.json is %s" % (server_version, version))
    # The last byte is projectLoaded: this host was started with --project
    check(payload[-1] == 1, "Hello: the host reports no project loaded")
    say("Hello: protocol %s, project loaded" % server_version)

    # The event ring kept the startup lines: RunHost constructs the server before Application::Init, so lines Init
    # logs itself are there. PollEvents (0x05): epoch, afterSeq, maxEvents (uint32 each); Events (0x0C): epoch,
    # nextSeq, dropped (uint32 each), then the events as a json field, searched here as text
    response = conn.expect(f.POLL_EVENTS, f.u32(0) + f.u32(0) + f.u32(1024), f.EVENTS, "PollEvents")
    poll_epoch, poll_next_seq = struct.unpack_from("<II", response, 0)  # the place to read on from later
    events_text = response[12:].decode("utf-8", "replace")
    for line in ("Camera initialized", "Using Software renderer, headless (no window)"):
        check(line in events_text, "PollEvents: no '%s' among the startup events (was the server constructed after Init?)" % line)
    say("PollEvents: %d bytes, startup lines present" % len(response))

    # The software renderer runs with no window, even on a GPU-less runner
    response = conn.expect(f.GET_ENGINE_HEALTH, b"", f.ENGINE_HEALTH, "GetEngineHealth")
    say("GetEngineHealth: healthy=%d" % response[0])
    states = subsystem_states(response)
    check(states.get("Renderer") == "Running", "The software renderer isn't running: %s" % states.get("Renderer"))
    check(states.get("Window") == "Disabled", "Expected no window (Disabled), got: %s" % states.get("Window"))

    # A frame: FrameData (0x02) is width, height (uint32), then width * height RGBA pixels, alpha 255
    width, height = 64, 48
    conn.expect(f.SET_VIEWPORT_SIZE, f.i32(width) + f.i32(height), f.OK, "SetViewportSize")
    payload = conn.expect(f.RENDER_FRAME, b"", f.FRAME_DATA, "RenderFrame")
    check(len(payload) == 8 + width * height * 4, "RenderFrame: %d bytes, expected %d" % (len(payload), 8 + width * height * 4))
    frame_width, frame_height = struct.unpack_from("<II", payload, 0)
    check((frame_width, frame_height) == (width, height),
          "RenderFrame: a %dx%d frame, expected %dx%d" % (frame_width, frame_height, width, height))
    for i in range(8 + 3, len(payload), 4):
        check(payload[i] == 255, "RenderFrame: pixel %d has alpha %d" % ((i - 11) // 4, payload[i]))
    say("RenderFrame: %dx%d, %d bytes of pixels" % (frame_width, frame_height, len(payload) - 8))

    # Headless audio runs on an OpenAL loopback device (no sound card needed), so GetAudio answers with AudioSamples:
    # sampleRate, channels, sampleFormat (string), frameCount, droppedFrames, samples
    payload = conn.expect(f.GET_AUDIO, b"", f.AUDIO_SAMPLES, "GetAudio")
    reader = f.Reader(payload)
    rate, channels, audio_format, frames = reader.u32(), reader.u32(), reader.string(), reader.u32()
    check(rate == 48000 and channels == 2, "Unexpected audio format: %d Hz, %d channels" % (rate, channels))
    say("GetAudio: %d Hz, %d channels, %s, %d frames" % (rate, channels, audio_format, frames))

    # The startup scene is open: GetOpenScene (0x29). An entity (CreateEntity, 0x30) leaves it unsaved;
    # SaveSceneToFile (0x26) with an empty path writes it to its own file; OpenScene (0x25) reads it back
    opened = scene_info(conn, f.GET_OPEN_SCENE, b"", "GetOpenScene")
    check(opened["path"] == "res://scenes/Main.scene" and opened["revision"] == opened["saved"],
          "The startup scene isn't open, or opened unsaved")
    conn.expect(f.CREATE_ENTITY, f.wire_string("Smoke Box"), f.ENTITY_CREATED, "CreateEntity")
    edited = scene_info(conn, f.GET_OPEN_SCENE, b"", "GetOpenScene after CreateEntity")
    check(edited["revision"] != edited["saved"], "CreateEntity left the scene saved")
    saved = scene_info(conn, f.SAVE_SCENE_TO_FILE, f.wire_string(""), "SaveSceneToFile")
    check(saved["revision"] == saved["saved"] and saved["uuid"] == opened["uuid"], "SaveSceneToFile didn't save the scene")
    check("Smoke Box" in read_text(project / "assets" / "scenes" / "Main.scene"), "Main.scene doesn't hold the saved entity")
    reopened = scene_info(conn, f.OPEN_SCENE, f.wire_string("res://scenes/Main.scene"), "OpenScene")
    check(reopened["uuid"] == opened["uuid"], "OpenScene gave the scene another UUID")
    # GetAllEntities: count, then (id, name) strings
    response = conn.expect(f.GET_ALL_ENTITIES, b"", f.ENTITY_LIST, "GetAllEntities")
    check("Smoke Box" in response.decode("utf-8", "replace"), "The reopened scene lost its entity")
    # A frame of the reopened scene, on the software renderer
    conn.expect(f.RENDER_FRAME, b"", f.FRAME_DATA, "RenderFrame after OpenScene")

    # The hierarchy. CreateEntityEx and DuplicateEntity (0x38: an id) answer EntityCreated (0x06): the id. GetEntity
    # (0x39: an id); EntityData (0x10) answers the entity as json, then its world matrix (16 float32).
    hierarchy_before = get_hierarchy(conn)
    check("Smoke Box" in hierarchy_before["names"],
          "GetHierarchy: the reopened scene lost its entity: %s" % ", ".join(hierarchy_before["names"]))
    cube_id = new_entity(conn, "Smoke Cube", "", -1, "Cube")
    lamp_id = new_entity(conn, "Smoke Lamp", cube_id, -1, "PointLight")
    hierarchy = get_hierarchy(conn)
    names = hierarchy["names"]
    cube_index = names.index("Smoke Cube") if "Smoke Cube" in names else -1
    check(cube_index >= 0 and cube_index + 1 < len(names) and names[cube_index + 1] == "Smoke Lamp",
          "GetHierarchy: the lamp doesn't follow its parent: %s" % ", ".join(names))
    lamp_node = hierarchy["nodes"][cube_index + 1]
    components = lamp_node.get("components") or []
    check(lamp_node.get("id") == lamp_id and lamp_node.get("parentId") == cube_id and components[:1] == ["Light"],
          "GetHierarchy: the lamp's node is wrong")
    check(hierarchy["revision"] > hierarchy_before["revision"], "GetHierarchy: the revision didn't move with CreateEntityEx")

    # A duplicate takes its subtree along, and is named after the original
    conn.expect(f.DUPLICATE_ENTITY, f.wire_string(cube_id), f.ENTITY_CREATED, "DuplicateEntity")
    duplicated = get_hierarchy(conn)
    check("Smoke Cube (1)" in duplicated["names"] and duplicated["names"].count("Smoke Lamp") == 2,
          "DuplicateEntity: %s" % ", ".join(duplicated["names"]))

    response = conn.expect(f.GET_ENTITY, f.wire_string(cube_id), f.ENTITY_DATA, "GetEntity")
    reader = f.Reader(response)
    entity = json.loads(reader.string())
    entity_components = entity.get("components") or []
    check(entity.get("header", {}).get("name") == "Smoke Cube" and entity_components[:1] and
          entity_components[0].get("type") == "CubeRenderer" and entity.get("transform") is not None,
          "GetEntity: the cube isn't what CreateEntityEx made")
    check(reader.remaining() == 64, "GetEntity: %d bytes of world matrix, expected 64" % reader.remaining())
    # Column-major: a new object at the origin has the identity, so element 15 (and the diagonal) is 1
    for element in (0, 5, 10, 15):
        (value,) = struct.unpack_from("<f", response, reader.offset + 4 * element)
        check(value == 1.0, "GetEntity: world matrix element %d isn't 1" % element)
    say("Hierarchy: %s" % ", ".join(duplicated["names"]))

    # sceneChanged says which objects changed (and the editor viewport still renders)
    response = conn.expect(f.POLL_EVENTS, f.u32(poll_epoch) + f.u32(poll_next_seq) + f.u32(1024), f.EVENTS,
                           "PollEvents after the hierarchy edits")
    events_text = response[12:].decode("utf-8", "replace")
    check('"entityIds"' in events_text and cube_id in events_text, "PollEvents: no sceneChanged with the cube's id in entityIds")
    conn.expect(f.RENDER_FRAME, b"", f.FRAME_DATA, "RenderFrame with a cube and a light")

    # GetProjectInfo (0x70): rootPath, userDataPath (strings), then project.n2proj as JSON
    reader = f.Reader(conn.expect(f.GET_PROJECT_INFO, b"", f.PROJECT_INFO, "GetProjectInfo"))
    root_path, user_data_path = reader.string(), reader.string()
    project_json = json.loads(reader.string())
    check(project_json.get("projectId") == project_id,
          "GetProjectInfo: projectId %s, expected %s" % (project_json.get("projectId"), project_id))
    folder = "Smoke_Project-" + project_id[:8]
    check(re.search(r"[\\/]" + re.escape(folder) + r"$", user_data_path),
          "GetProjectInfo: user:// isn't the project's own folder: %s" % user_data_path)
    # Under the user data folder: %APPDATA%/N2Engine, or $XDG_DATA_HOME/n2engine, else $HOME/.local/share/n2engine
    check(same_path(Path(user_data_path).parent, user_data_base()),
          "GetProjectInfo: user:// %s isn't under the user data folder %s" % (user_data_path, user_data_base()))
    say("GetProjectInfo: %s, user:// at %s" % (root_path, user_data_path))

    # --project initialised the ResourceLoader: the startup asset has metadata, and an asset added afterwards gets
    # its own on RescanAssets
    assert_meta(project, "startup.mat")
    (project / "assets" / "added.mat").write_text("{}")
    conn.expect(f.RESCAN_ASSETS, b"", f.OK, "RescanAssets")
    assert_meta(project, "added.mat")

    conn.expect(f.SHUTDOWN, b"", f.OK, "Shutdown")
    conn.close()
    check_exit_zero(proc, 30, "Editor host (after Shutdown)")
    say("Editor host exited cleanly")
    log = read_text(proc.n2_out) + read_text(proc.n2_err)
    for problem in ("Assets directory not found", "ResourceUUID not initialized", "Couldn't open the startup scene"):
        check(problem not in log, "Host log reports: " + problem)
    check(token not in log, "The host logged its access token")

    # ---- Host 2: the default renderer (no GPU here, so no window), --exit-on-disconnect, no token ----
    proc2 = hosts.start("host-exit-on-disconnect", ["--port", "0", "--exit-on-disconnect"])
    port = int(wait_ready_line(proc2).group(1))
    conn = f.connect(port)
    conn.expect(f.HELLO, f.hello_payload("CI smoke test", version, ""), f.SERVER_INFO, "Hello without a token")
    conn.expect(f.GET_ENGINE_HEALTH, b"", f.ENGINE_HEALTH, "GetEngineHealth")
    # Leaving without Shutdown: the session ends, so the host stops by itself
    conn.close()
    check_exit_zero(proc2, 30, "Editor host with --exit-on-disconnect (client left)")
    say("Editor host with --exit-on-disconnect exited cleanly when its client left")

    # ---- --exit-on-stdin-eof, on a real pipe: running while stdin is open, gone once it closes ----
    proc_pipe = hosts.start("host-stdin-pipe", ["--port", "0", "--exit-on-stdin-eof"], stdin=subprocess.PIPE)
    time.sleep(5)
    check(proc_pipe.poll() is None,
          "Editor host with --exit-on-stdin-eof exited while its stdin was still open (code %s)" % proc_pipe.returncode)
    proc_pipe.stdin.close()
    check_exit_zero(proc_pipe, 30, "Editor host with --exit-on-stdin-eof (stdin pipe closed)")
    say("Editor host with --exit-on-stdin-eof exited cleanly when its stdin pipe closed")

    # ---- --exit-on-stdin-eof, stdin already at end of file (an empty file) ----
    empty_stdin = work / "empty-stdin.txt"
    empty_stdin.write_bytes(b"")
    with open(empty_stdin, "rb") as empty:
        proc_eof = hosts.start("host-stdin-eof", ["--port", "0", "--exit-on-stdin-eof"], stdin=empty)
    check_exit_zero(proc_eof, 30, "Editor host with --exit-on-stdin-eof (stdin at end of file)")
    say("Editor host with --exit-on-stdin-eof exited cleanly with its stdin at end of file")
    # The Logger writes to the console in Debug builds only
    if config == "Debug":
        check("Stdin closed" in read_text(proc_eof.n2_out), "The host's log doesn't say its stdin closed")

    # ---- Without the flag, the same stdin changes nothing: the host keeps serving ----
    with open(empty_stdin, "rb") as empty:
        proc_no_flag = hosts.start("host-no-stdin-flag", ["--port", "0"], stdin=empty)
    wait_ready_line(proc_no_flag, what="Editor host without --exit-on-stdin-eof")
    time.sleep(3)
    check(proc_no_flag.poll() is None,
          "Editor host without --exit-on-stdin-eof exited when its stdin was at end of file (code %s)" % proc_no_flag.returncode)
    say("Editor host without --exit-on-stdin-eof kept running with its stdin at end of file")
    check("Stdin closed" not in read_text(proc_no_flag.n2_out),
          "A host without --exit-on-stdin-eof logged that its stdin closed")


# ---- Scenario: tls ----

def tls_round(hosts, name, version):
    """Starts a --tls host, pins its certificate, says Hello over TLS, and requires a clean exit once the client
    leaves. Returns the fingerprint from the ready line."""
    proc = hosts.start(name, ["--tls", "--port", "0", "--renderer", "software", "--exit-on-disconnect"])
    # The ready line, as a launcher reads it: port, tls=1 and the certificate's SHA-256 as 64 lowercase hex digits
    match = wait_ready_line(proc, TLS_READY_LINE, "The --tls host")
    port, fingerprint = int(match.group(1)), match.group(2)
    say("TLS host ready on port %d, fingerprint %s" % (port, fingerprint))

    # Accept any certificate in the handshake, then pin by comparing the fingerprint ourselves: the same
    # trust-on-first-use check a client makes
    conn = f.connect(port, tls=True)
    seen = hashlib.sha256(conn.sock.getpeercert(binary_form=True)).hexdigest()
    check(seen == fingerprint,
          "The certificate the host presented (%s) is not the one its ready line named (%s)" % (seen, fingerprint))
    say("Handshake done (%s); the fingerprint matches the ready line" % conn.sock.version())

    # Hello (0x04) over TLS: ServerInfo is 0x0B
    response_type, payload = conn.request(f.HELLO, f.hello_payload("CI TLS smoke test", version, ""))
    f.assert_response(response_type, payload, f.SERVER_INFO, "Hello over TLS")
    say("Hello over TLS answered with ServerInfo")

    conn.close()
    check_exit_zero(proc, 30, "The --tls host (client left)")
    say("The --tls host exited cleanly when its client left")
    return fingerprint


def scenario_tls(hosts, version):
    """The real host with --tls: it makes its certificate in the user's data folder (or reuses the one there),
    prints it in the ready line, and a client that checks that fingerprint finishes a handshake, says Hello and gets
    ServerInfo. A second host on the same folder presents the same certificate."""
    first = tls_round(hosts, "tls-host", version)

    # The certificate and key live in the user's data folder, not in the project or the repository
    identity = user_data_base() / "editor-tls"
    for name in ("host-cert.pem", "host-key.pem"):
        check((identity / name).is_file(), "No %s in %s" % (name, identity))
    check("PRIVATE KEY" not in read_text(identity / "host-cert.pem"), "The certificate file holds a private key")
    check("PRIVATE KEY" in read_text(identity / "host-key.pem"), "The key file holds no private key")
    if os.name != "nt":
        mode = stat.S_IMODE((identity / "host-key.pem").stat().st_mode)
        check(mode == 0o600, "host-key.pem has mode %04o, expected 0600" % mode)
        say("host-key.pem mode 0600")

    # One identity per user: another start keeps the certificate, so a fingerprint a client pinned stays valid
    second = tls_round(hosts, "tls-host-restart", version)
    check(second == first, "The restarted --tls host made a new certificate (%s), not the first one (%s)" % (second, first))
    say("The restarted --tls host reused the certificate")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--exe", required=True, help="path to N2EditorHost (N2EditorHost.exe on Windows)")
    parser.add_argument("--config", default="Release", help="Debug or Release: Debug builds log to the console")
    parser.add_argument("--scenario", choices=("host", "tls", "all"), default="all")
    parser.add_argument("--work-dir", help="folder for the project and the host logs (default: a new one under $RUNNER_TEMP or the system temp folder)")
    args = parser.parse_args()

    exe = Path(args.exe).resolve()
    check(exe.is_file(), "No editor host at %s" % exe)
    base = args.work_dir or os.environ.get("RUNNER_TEMP") or tempfile.gettempdir()
    work = Path(tempfile.mkdtemp(prefix="n2-smoke-", dir=base)).resolve()
    version = protocol_version()
    hosts = Hosts(exe, work)
    try:
        if args.scenario in ("host", "all"):
            scenario_host(hosts, work, args.config, version)
        if args.scenario in ("tls", "all"):
            scenario_tls(hosts, version)
    except Exception as error:  # noqa: BLE001
        hosts.kill_all()
        hosts.dump()
        say("SMOKE TEST FAILED: %s: %s" % (type(error).__name__, error))
        return 1
    hosts.kill_all()
    hosts.dump()
    say("Smoke test passed (%s)" % args.scenario)
    return 0


if __name__ == "__main__":
    sys.exit(main())
