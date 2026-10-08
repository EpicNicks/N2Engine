# The play child smoke test (#82, E9): what a launcher (the Electron editor) does for play mode, with the real
# N2EditorHost binary on the windowless software renderer. Run by the Tests workflow after the editor host smoke test:
#
#   pwsh -File tests/editor-server/PlaySmokeTest.ps1 -Config Debug
#
# 1. An edit host opens a project, a scripted object is added to its open scene (not saved), and WritePlaySnapshot
#    writes the scene as it is in memory. The scene's own file must stay as it was, and the edit host unchanged.
# 2. A second process, the play child, is started as N2EditorHost --project <same> --renderer software --play <snapshot>
#    --port 0 --token-env ... --exit-on-disconnect. Its ready line gives its port; the test connects and checks that
#    the script ran (its Debug.Log lines arrive as log events), that frames advance while a busy client sends commands
#    as fast as it can, SetPaused, Step, SendInput, RenderFrame, that the commands which write the project are
#    refused, and that Shutdown ends it with exit code 0.
# 3. The edit host answers as before (GetPlayState says Edit), and its scene file is still unchanged.
# 4. A play child whose snapshot is missing exits with code 1 before any ready line; one whose client leaves exits by
#    itself (--exit-on-disconnect).
param(
  [Parameter(Mandatory = $true)][string]$Config
)
$ErrorActionPreference = "Stop"

$exe = Resolve-Path "build/bin/$Config/N2EditorHost.exe"
$protocolVersion = (Get-Content editor-server/protocol/protocol.json -Raw | ConvertFrom-Json).version
$temp = if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { [System.IO.Path]::GetTempPath() }

# ---------------------------------------------------------------------------------------------------------------
# The same wire helpers as the editor host smoke test (frames are [type: 1 byte][payload length: uint32 LE][payload])
# ---------------------------------------------------------------------------------------------------------------
function Wait-ReadyLine($proc, [string]$logPath) {
  $port = $null
  $text = ""
  for ($i = 0; $i -lt 240 -and -not $port; $i++) {
    try {
      $file = [System.IO.File]::Open($logPath, 'Open', 'Read', 'ReadWrite')
      try { $text = [System.IO.StreamReader]::new($file).ReadToEnd() } finally { $file.Dispose() }
    } catch { }
    $end = $text.LastIndexOf("`n")
    if ($end -ge 0) {
      foreach ($line in ($text.Substring(0, $end + 1) -split "`n")) {
        if ($line.TrimEnd("`r") -match '^N2EditorHost ready (?:.* )?port=(\d+)(?: |$)') { $port = [int]$Matches[1]; break }
      }
    }
    if (-not $port) {
      if ($proc.HasExited) { throw "Host exited (code $($proc.ExitCode)) before its ready line" }
      Start-Sleep -Milliseconds 500
    }
  }
  if (-not $port) { throw "Host never printed its ready line" }
  if ($port -lt 1 -or $port -gt 65535) { throw "Ready line has an invalid port: $port" }
  return $port
}

function Connect-Host([int]$port) {
  $client = $null
  for ($i = 0; $i -lt 10 -and -not $client; $i++) {
    try { $client = [System.Net.Sockets.TcpClient]::new("127.0.0.1", $port) }
    catch { Start-Sleep -Milliseconds 500 }
  }
  if (-not $client) { throw "Host never accepted a connection on port $port" }
  $client.ReceiveTimeout = 15000
  $client.NoDelay = $true
  return $client
}

function Send-Frame($stream, [byte]$type, [byte[]]$payload = @()) {
  $bytes = [byte[]](@($type) + [BitConverter]::GetBytes([uint32]$payload.Length) + $payload)
  $stream.Write($bytes, 0, $bytes.Length)
}
function Read-Exact($stream, [int]$count) {
  $buffer = New-Object byte[] $count
  $offset = 0
  while ($offset -lt $count) {
    $read = $stream.Read($buffer, $offset, $count - $offset)
    if ($read -le 0) { throw "Connection closed after $offset of $count bytes" }
    $offset += $read
  }
  return ,$buffer
}
function Read-Response($stream) {
  $header = Read-Exact $stream 5
  $payload = Read-Exact $stream ([BitConverter]::ToUInt32($header, 1))
  return @{ Type = $header[0]; Payload = $payload }
}
# Sends a command and reads its response
function Send-Command($stream, [byte]$type, [byte[]]$payload = @()) {
  Send-Frame $stream $type $payload
  return Read-Response $stream
}
function Assert-Response($response, [byte]$expected, [string]$what) {
  if ($response.Type -ne $expected) {
    throw ("{0}: expected response 0x{1:X2}, got 0x{2:X2}: {3}" -f $what, $expected, $response.Type,
      [Text.Encoding]::UTF8.GetString($response.Payload))
  }
}
function Assert-Error($response, [string]$what, [string]$contains) {
  if ($response.Type -ne 0x01) { throw "${what}: expected an Error, got response 0x$('{0:X2}' -f $response.Type)" }
  $message = [Text.Encoding]::UTF8.GetString($response.Payload)
  if (-not $message.Contains($contains)) { throw "${what}: the Error '$message' doesn't mention '$contains'" }
}

function Get-StringBytes([string]$text) {
  $utf8 = [Text.Encoding]::UTF8.GetBytes($text)
  return ,[byte[]]([BitConverter]::GetBytes([uint32]$utf8.Length) + $utf8)
}
function Read-WireString([byte[]]$payload, [ref]$offset) {
  $length = [BitConverter]::ToUInt32($payload, $offset.Value)
  $text = [Text.Encoding]::UTF8.GetString($payload, $offset.Value + 4, $length)
  $offset.Value += 4 + $length
  return $text
}

function Connect-Session([int]$port, [string]$token) {
  $client = Connect-Host $port
  $stream = $client.GetStream()
  $hello = (Get-StringBytes "play smoke test") + (Get-StringBytes $protocolVersion) + (Get-StringBytes $token)
  $response = Send-Command $stream 0x04 ([byte[]]$hello)
  Assert-Response $response 0x0B "Hello"
  $offset = 0
  $serverVersion = Read-WireString $response.Payload ([ref]$offset)
  if ($serverVersion -ne $protocolVersion) { throw "The host speaks protocol $serverVersion, protocol.json is $protocolVersion" }
  return @{ Client = $client; Stream = $stream }
}

# GetPlayState (0xB3) answers PlayState (0xB1): state (string), frame (uint32), time (float32)
function Get-PlayState($stream) {
  $response = Send-Command $stream 0xB3
  Assert-Response $response 0xB1 "GetPlayState"
  $offset = 0
  $state = Read-WireString $response.Payload ([ref]$offset)
  $frame = [BitConverter]::ToUInt32($response.Payload, $offset)
  $time = [BitConverter]::ToSingle($response.Payload, $offset + 4)
  return @{ State = $state; Frame = $frame; Time = $time }
}

# PollEvents (0x05): epoch, afterSeq, maxEvents (uint32 each) -> Events (0x0C): epoch, nextSeq, dropped, then the
# events as a json string. Accumulates the events' text per host in $script:EventText, reading on from where it left.
$script:EventText = @{}
$script:EventCursor = @{}
function Read-Events($stream, [string]$host_) {
  if (-not $script:EventCursor.ContainsKey($host_)) { $script:EventCursor[$host_] = @(0, 0); $script:EventText[$host_] = "" }
  $cursor = $script:EventCursor[$host_]
  $request = [byte[]]([BitConverter]::GetBytes([uint32]$cursor[0]) + [BitConverter]::GetBytes([uint32]$cursor[1]) + [BitConverter]::GetBytes([uint32]1024))
  $response = Send-Command $stream 0x05 $request
  Assert-Response $response 0x0C "PollEvents"
  $script:EventCursor[$host_] = @([BitConverter]::ToUInt32($response.Payload, 0), [BitConverter]::ToUInt32($response.Payload, 4))
  $script:EventText[$host_] += [Text.Encoding]::UTF8.GetString($response.Payload, 12, $response.Payload.Length - 12)
  return $script:EventText[$host_]
}
function Wait-ForEvent($stream, [string]$host_, [string]$needle, [int]$seconds) {
  $watch = [System.Diagnostics.Stopwatch]::StartNew()
  while ($watch.Elapsed.TotalSeconds -lt $seconds) {
    if ((Read-Events $stream $host_).Contains($needle)) { return }
    Start-Sleep -Milliseconds 100
  }
  throw "No '$needle' event from the $host_ host within $seconds s"
}

function Start-Host([string[]]$arguments, [string]$name, [string]$token) {
  $log = Join-Path $PWD "$name.log"
  $err = Join-Path $PWD "$name.err"
  if ($token) { $env:N2_PLAY_SMOKE_TOKEN = $token }
  try {
    $proc = Start-Process -FilePath $exe -PassThru -ArgumentList $arguments -RedirectStandardOutput $log -RedirectStandardError $err
  } finally {
    Remove-Item Env:N2_PLAY_SMOKE_TOKEN -ErrorAction SilentlyContinue
  }
  $null = $proc.Handle # keeps ExitCode readable after the process exits
  return @{ Process = $proc; Log = $log; Err = $err }
}

function Wait-Exit($host_, [string]$what, [int]$seconds = 20) {
  if (-not $host_.Process.WaitForExit($seconds * 1000)) { throw "$what didn't exit within $seconds s" }
  return $host_.Process.ExitCode
}

# ---------------------------------------------------------------------------------------------------------------
# A project with a script that logs from its first attach and its third update
# ---------------------------------------------------------------------------------------------------------------
$project = Join-Path $temp "n2 play smoke"
Remove-Item $project -Recurse -Force -ErrorAction SilentlyContinue
$create = Start-Process -FilePath $exe -PassThru -Wait -NoNewWindow -ArgumentList "--create", "`"$project`"", "--name", "`"Play Smoke`"" `
  -RedirectStandardOutput (Join-Path $PWD "play-create.out") -RedirectStandardError (Join-Path $PWD "play-create.err")
if ($create.ExitCode -ne 0) { throw "--create exited with $($create.ExitCode)" }

Set-Content (Join-Path $project "assets/scripts/PlaySmoke.lua") @'
local PlaySmoke = {}
PlaySmoke.__index = PlaySmoke

local updates = 0

function PlaySmoke:OnAttach()
  Debug.Log("PLAYSMOKE_ATTACH")
end

function PlaySmoke:OnUpdate()
  updates = updates + 1
  if updates == 3 then
    Debug.Log("PLAYSMOKE_UPDATE_3")
  end
end

return PlaySmoke
'@

$sceneFile = Join-Path $project "assets/scenes/Main.scene"
$sceneBefore = Get-Content $sceneFile -Raw
$token = [guid]::NewGuid().ToString("N")
$editHost = $null
$playHost = $null
$exitHost = $null
$missingHost = $null

try {
  # ---- The host that edits ----
  $editHost = Start-Host @("--port", "0", "--project", "`"$project`"", "--renderer", "software", "--token-env", "N2_PLAY_SMOKE_TOKEN") "play-edit-host" $token
  $editPort = Wait-ReadyLine $editHost.Process $editHost.Log
  $edit = Connect-Session $editPort $token
  $editStream = $edit.Stream

  $state = Get-PlayState $editStream
  if ($state.State -ne "Edit" -or $state.Frame -ne 0) { throw "The edit host reports play state '$($state.State)' frame $($state.Frame), expected Edit, 0" }
  foreach ($play in @(@(0xB1, [byte[]](1)), @(0xB2, [byte[]]([BitConverter]::GetBytes([uint32]1))), @(0xB4, (Get-StringBytes "[]")))) {
    Assert-Error (Send-Command $editStream $play[0] $play[1]) "Play command 0x$('{0:X2}' -f $play[0]) on the edit host" "Not a play host"
  }
  Write-Host "Edit host: GetPlayState says Edit; SetPaused, Step and SendInput refuse"

  # A scripted object in the open scene, which is not saved: CreateEntityEx (0x35: name, parentId, siblingIndex int32,
  # preset) answers EntityCreated (0x06: the object's UUID); AddComponent (0x61: entityId, type) answers ComponentAdded
  # (0x12: componentId, ...); SetComponentFields (0x63: entityId, componentId, values json) answers ComponentData
  $create = [byte[]]((Get-StringBytes "Scripted") + (Get-StringBytes "") + [BitConverter]::GetBytes([int32]-1) + (Get-StringBytes "Empty"))
  $response = Send-Command $editStream 0x35 $create
  Assert-Response $response 0x06 "CreateEntityEx"
  $offset = 0
  $entity = Read-WireString $response.Payload ([ref]$offset)
  $response = Send-Command $editStream 0x61 ([byte[]]((Get-StringBytes $entity) + (Get-StringBytes "LuaComponent")))
  Assert-Response $response 0x12 "AddComponent LuaComponent"
  $offset = 0
  $component = Read-WireString $response.Payload ([ref]$offset)
  $meta = Join-Path $project ".import/scripts/PlaySmoke.lua.meta"
  if (-not (Test-Path $meta)) { throw "The host made no .meta for scripts/PlaySmoke.lua at $meta" }
  $scriptUuid = (Get-Content $meta -Raw | ConvertFrom-Json).uuid
  $values = '{"scriptUUID":"' + $scriptUuid + '"}'
  $response = Send-Command $editStream 0x63 ([byte[]]((Get-StringBytes $entity) + (Get-StringBytes $component) + (Get-StringBytes $values)))
  Assert-Response $response 0x13 "SetComponentFields scriptUUID"
  Write-Host "The open scene has an unsaved scripted object ($entity)"

  # GetOpenScene (0x29): SceneInfo (0x0D): path, name, uuid (strings), revision, savedRevision (uint32)
  function Get-Revisions($stream) {
    $response = Send-Command $stream 0x29
    Assert-Response $response 0x0D "GetOpenScene"
    $o = 0
    $null = Read-WireString $response.Payload ([ref]$o); $null = Read-WireString $response.Payload ([ref]$o); $null = Read-WireString $response.Payload ([ref]$o)
    return @{ Revision = [BitConverter]::ToUInt32($response.Payload, $o); Saved = [BitConverter]::ToUInt32($response.Payload, $o + 4) }
  }
  $revisionsBefore = Get-Revisions $editStream
  if ($revisionsBefore.Revision -eq $revisionsBefore.Saved) { throw "The scene should have unsaved changes" }

  # WritePlaySnapshot (0xB0: scenePath, empty for the open scene) answers PlaySnapshot (0xB0: file)
  $response = Send-Command $editStream 0xB0 (Get-StringBytes "")
  Assert-Response $response 0xB0 "WritePlaySnapshot"
  $offset = 0
  $snapshot = Read-WireString $response.Payload ([ref]$offset)
  if (-not (Test-Path $snapshot)) { throw "The snapshot file $snapshot doesn't exist" }
  if (-not [System.IO.Path]::IsPathRooted($snapshot)) { throw "The snapshot path isn't absolute: $snapshot" }
  $snapshotText = Get-Content $snapshot -Raw
  if (-not $snapshotText.Contains("LuaComponent") -or -not $snapshotText.Contains($scriptUuid)) { throw "The snapshot doesn't hold the unsaved scripted object" }
  if ((Get-Content $sceneFile -Raw) -ne $sceneBefore) { throw "WritePlaySnapshot changed the scene's own file" }
  $revisionsAfter = Get-Revisions $editStream
  if ($revisionsAfter.Revision -ne $revisionsBefore.Revision -or $revisionsAfter.Saved -ne $revisionsBefore.Saved) { throw "WritePlaySnapshot changed the scene's revisions" }
  Write-Host "WritePlaySnapshot: $snapshot (the scene file and revisions are untouched)"

  # ---- The play child, started as the launcher starts it ----
  $playHost = Start-Host @("--port", "0", "--project", "`"$project`"", "--renderer", "software", "--token-env", "N2_PLAY_SMOKE_TOKEN",
    "--play", "`"$snapshot`"", "--exit-on-disconnect") "play-child-host" $token
  $playPort = Wait-ReadyLine $playHost.Process $playHost.Log
  if ($playPort -eq $editPort) { throw "The play child listens on the edit host's port" }
  $playSession = Connect-Session $playPort $token
  $playStream = $playSession.Stream
  Write-Host "Play child ready on port $playPort"

  $state = Get-PlayState $playStream
  if ($state.State -ne "Playing") { throw "The play child's state is '$($state.State)', expected Playing" }

  # The script (attached at the first frame) ran in the child: its Debug.Log lines are log events
  Wait-ForEvent $playStream "play" "PLAYSMOKE_ATTACH" 20
  Wait-ForEvent $playStream "play" "PLAYSMOKE_UPDATE_3" 20
  if (-not $script:EventText["play"].Contains('"playState"')) { throw "The play child sent no playState event" }
  Write-Host "The scripted object ran in the child: attach, then update 3"

  # Frames advance on their own, at about 60 a second
  $first = Get-PlayState $playStream
  Start-Sleep -Milliseconds 500
  $second = Get-PlayState $playStream
  if ($second.Frame -lt $first.Frame + 5) { throw "The game ran $($second.Frame - $first.Frame) frames in 0.5 s" }
  if ($second.Time -le $first.Time) { throw "The game's time didn't advance ($($first.Time) -> $($second.Time))" }
  Write-Host "Playing: $($second.Frame - $first.Frame) frames in 0.5 s"

  # A client that sends commands as fast as it can doesn't starve the game, and the game doesn't starve it
  $before = Get-PlayState $playStream
  $watch = [System.Diagnostics.Stopwatch]::StartNew()
  $answered = 0
  while ($watch.Elapsed.TotalSeconds -lt 1.5) { $null = Get-PlayState $playStream; $answered++ }
  $after = Get-PlayState $playStream
  $seconds = $watch.Elapsed.TotalSeconds
  if ($answered -lt 20) { throw "The busy client got $answered answers in $([int]$seconds) s" }
  if (($after.Frame - $before.Frame) -lt (10 * $seconds)) { throw "A busy client left the game $($after.Frame - $before.Frame) frames in $([int]$seconds) s" }
  Write-Host "Busy client: $answered answers and $($after.Frame - $before.Frame) frames in $([math]::Round($seconds, 1)) s"

  # RenderFrame draws the game: SetViewportSize (0x02: width, height int32), then FrameData (0x02)
  Assert-Response (Send-Command $playStream 0x02 ([byte[]]([BitConverter]::GetBytes([int32]64) + [BitConverter]::GetBytes([int32]48)))) 0x00 "SetViewportSize"
  $timeBefore = (Get-PlayState $playStream).Time
  $response = Send-Command $playStream 0x01
  Assert-Response $response 0x02 "RenderFrame in the play child"
  if ($response.Payload.Length -ne 8 + 64 * 48 * 4) { throw "RenderFrame in the play child: $($response.Payload.Length) bytes" }
  for ($i = 8 + 3; $i -lt $response.Payload.Length; $i += 4) { if ($response.Payload[$i] -ne 255) { throw "RenderFrame: a pixel isn't opaque" } }
  Write-Host "RenderFrame: the game, 64x48"
  # RenderFrameIfChanged (0x06) is the editor view: refused
  Assert-Error (Send-Command $playStream 0x06 ([byte[]]([BitConverter]::GetBytes([uint32]0)))) "RenderFrameIfChanged in the play child" "RenderFrame"

  # The commands that write the project are refused: OpenScene (0x25), SaveSceneToFile (0x26), SetStartupScene (0x72)
  foreach ($command in 0x25, 0x26, 0x72) {
    Assert-Error (Send-Command $playStream $command (Get-StringBytes "res://scenes/Main.scene")) "Command 0x$('{0:X2}' -f $command) in the play child" "play host"
  }
  Write-Host "The play child refuses OpenScene, SaveSceneToFile and SetStartupScene"

  # SendInput (0xB4: events json): good events are accepted, a bad batch is refused whole
  Assert-Response (Send-Command $playStream 0xB4 (Get-StringBytes '[{"type":"key","key":"W","down":true},{"type":"pointer","x":10,"y":5},{"type":"mouseButton","button":"Left","down":true},{"type":"scroll","x":0,"y":1},{"type":"releaseAll"}]')) 0x00 "SendInput"
  Assert-Error (Send-Command $playStream 0xB4 (Get-StringBytes '[{"type":"key","key":"W","down":true},{"type":"key","key":"NotAKey","down":true}]')) "SendInput with a bad key" "events[1]"

  # SetPaused (0xB1: bool): paused, the frame count stops; Step (0xB2: uint32) runs frames; resuming goes on
  Assert-Response (Send-Command $playStream 0xB1 ([byte[]](1))) 0x00 "SetPaused true"
  $paused = Get-PlayState $playStream
  if ($paused.State -ne "Paused") { throw "After SetPaused true the state is '$($paused.State)'" }
  Start-Sleep -Milliseconds 400
  $stillPaused = Get-PlayState $playStream
  if ($stillPaused.Frame -ne $paused.Frame) { throw "A paused game ran $($stillPaused.Frame - $paused.Frame) frames" }
  Assert-Response (Send-Command $playStream 0xB2 ([byte[]]([BitConverter]::GetBytes([uint32]5)))) 0x00 "Step 5"
  $stepped = Get-PlayState $playStream
  if ($stepped.Frame -ne $paused.Frame + 5 -or $stepped.State -ne "Paused") { throw "Step 5: frame $($paused.Frame) -> $($stepped.Frame), state $($stepped.State)" }
  $stepSeconds = $stepped.Time - $paused.Time
  if ([math]::Abs($stepSeconds - 0.1) -gt 0.001) { throw "Five steps advanced the game's time by $stepSeconds s, expected 0.1" }
  Assert-Error (Send-Command $playStream 0xB2 ([byte[]]([BitConverter]::GetBytes([uint32]0)))) "Step 0" "frames"
  Assert-Response (Send-Command $playStream 0xB1 ([byte[]](0))) 0x00 "SetPaused false"
  Start-Sleep -Milliseconds 400
  $resumed = Get-PlayState $playStream
  if ($resumed.State -ne "Playing" -or $resumed.Frame -le $stepped.Frame + 3) { throw "Resumed: state $($resumed.State), frame $($stepped.Frame) -> $($resumed.Frame)" }
  Write-Host "SetPaused and Step: paused at $($paused.Frame), stepped 5 frames (0.1 s), resumed at about 60 frames a second"

  # Shutdown (0xFF) ends the child with exit code 0
  Assert-Response (Send-Command $playStream 0xFF) 0x00 "Shutdown of the play child"
  $playSession.Client.Close()
  $code = Wait-Exit $playHost "The play child"
  if ($code -ne 0) { throw "The play child exited with code $code" }
  Write-Host "Shutdown: the play child exited with code 0"

  # ---- The host that edits is as it was ----
  $state = Get-PlayState $editStream
  if ($state.State -ne "Edit") { throw "The edit host reports '$($state.State)' after the child ended" }
  $revisionsEnd = Get-Revisions $editStream
  if ($revisionsEnd.Revision -ne $revisionsBefore.Revision -or $revisionsEnd.Saved -ne $revisionsBefore.Saved) { throw "The edit scene changed while the child played" }
  if ((Get-Content $sceneFile -Raw) -ne $sceneBefore) { throw "The scene file changed while the child played" }
  Write-Host "The edit host and its scene are unchanged"

  # ---- A child whose client leaves exits by itself (--exit-on-disconnect) ----
  $exitHost = Start-Host @("--port", "0", "--project", "`"$project`"", "--renderer", "software", "--token-env", "N2_PLAY_SMOKE_TOKEN",
    "--play", "`"$snapshot`"", "--exit-on-disconnect") "play-child-exit" $token
  $exitPort = Wait-ReadyLine $exitHost.Process $exitHost.Log
  $exitSession = Connect-Session $exitPort $token
  $null = Get-PlayState $exitSession.Stream
  $exitSession.Client.Close()
  $code = Wait-Exit $exitHost "A play child whose client left"
  if ($code -ne 0) { throw "A play child whose client left exited with code $code" }
  Write-Host "--exit-on-disconnect: the play child left with its client (exit code 0)"

  # ---- A snapshot that can't be played ends the host before it listens ----
  $missing = Join-Path $project ".n2/play/does-not-exist.scene"
  $missingHost = Start-Host @("--port", "0", "--project", "`"$project`"", "--renderer", "software", "--play", "`"$missing`"") "play-child-missing" ""
  $code = Wait-Exit $missingHost "A play child with a missing snapshot"
  if ($code -ne 1) { throw "A play child with a missing snapshot exited with code $code, expected 1" }
  if ((Get-Content $missingHost.Log -Raw) -match 'N2EditorHost ready') { throw "A play child with a missing snapshot printed a ready line" }
  $missingErr = Get-Content $missingHost.Err -Raw
  if (-not "$missingErr".Contains("--play")) { throw "A play child with a missing snapshot said nothing about --play on stderr: $missingErr" }
  Write-Host "A missing snapshot: exit code 1, no ready line"

  # ---- The host that edits shuts down cleanly ----
  Assert-Response (Send-Command $editStream 0xFF) 0x00 "Shutdown of the edit host"
  $edit.Client.Close()
  $code = Wait-Exit $editHost "The edit host"
  if ($code -ne 0) { throw "The edit host exited with code $code" }
  Write-Host "Play smoke test passed"
}
finally {
  foreach ($h in $editHost, $playHost, $exitHost, $missingHost) {
    if ($h -and -not $h.Process.HasExited) { $h.Process.Kill() }
  }
  foreach ($h in $editHost, $playHost, $exitHost, $missingHost) {
    if ($h) {
      Write-Host "---- $($h.Log) ----"
      Get-Content $h.Log -ErrorAction SilentlyContinue | Select-Object -Last 30 | ForEach-Object { Write-Host $_ }
      Get-Content $h.Err -ErrorAction SilentlyContinue | ForEach-Object { Write-Host "stderr: $_" }
    }
  }
}
