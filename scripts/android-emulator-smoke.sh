#!/usr/bin/env bash
# The Android emulator smoke test (build targets stage 8; docs/android.html): installs an APK n2build made, starts the
# game, and passes when the player logs its first frame and then a frame-rate line, with no crash of the app's process.
#
#   scripts/android-emulator-smoke.sh <package folder> [seconds]
#
# The package folder is n2build's (it holds .n2build-package.json, whose appId and entry name the app and the APK).
# Needs adb on PATH and one running device or emulator.
set -euo pipefail

package="${1:?usage: android-emulator-smoke.sh <package folder> [seconds]}"
seconds="${2:-45}"
marker="$package/.n2build-package.json"
test -f "$marker" || { echo "::error::$marker is missing: not a package n2build made"; exit 1; }
app_id="$(node -e 'console.log(JSON.parse(require("fs").readFileSync(process.argv[1], "utf8")).appId)' "$marker")"
entry="$(node -e 'console.log(JSON.parse(require("fs").readFileSync(process.argv[1], "utf8")).entry)' "$marker")"
apk="$package/$entry"
test -f "$apk" || { echo "::error::$apk is missing"; exit 1; }

adb wait-for-device
# The emulator reports "booted" before the package manager is ready
for _ in $(seq 1 60); do
  if adb shell pm path android >/dev/null 2>&1; then break; fi
  sleep 2
done

adb logcat -c
adb install -r "$apk"
adb shell am start -W -n "$app_id/com.n2engine.player.N2Activity"

log="$(mktemp)"
passed=0
for _ in $(seq 1 "$seconds"); do
  sleep 1
  adb logcat -d -v brief > "$log" || true
  if grep -q "N2Engine Android player: first frame" "$log" && grep -Eq "N2Engine Android player: [0-9]+ frames, [0-9.]+ fps" "$log"; then
    passed=1
    break
  fi
  if grep -Eq "FATAL EXCEPTION|Fatal signal|Process $app_id .* has died|$app_id.*crashed" "$log"; then
    break
  fi
done

echo "--- the game's log (tag N2Engine) ---"
grep -E "/N2Engine\(" "$log" || true
pid="$(adb shell pidof "$app_id" | tr -d '\r' || true)"
if [ "$passed" != 1 ]; then
  echo "--- crashes and the runtime's errors ---"
  grep -E "FATAL|Fatal signal|AndroidRuntime|DEBUG|libc|$app_id" "$log" | tail -200 || true
  echo "::error::The game did not log a first frame and a frame rate within ${seconds}s (process: ${pid:-not running})"
  exit 1
fi
if [ -z "$pid" ]; then
  echo "::error::The game logged frames but is no longer running"
  exit 1
fi
grep -E "N2Engine Android player: [0-9]+ frames" "$log" | tail -1
adb shell am force-stop "$app_id"
echo "The game ran on the emulator (pid $pid)"
