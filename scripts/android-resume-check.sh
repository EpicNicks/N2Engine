#!/usr/bin/env bash
# Background and back (build targets stage 12; docs/android.html#vulkan): starts the game, sends it home (Android frees its
# window), brings it back, and passes when the same process is still running and logs frames after the resume, so the
# renderer let go of the old surface and drew on the new one.
#
#   scripts/android-resume-check.sh <app id>
set -euo pipefail

app="${1:?usage: android-resume-check.sh <app id>}"
activity="$app/com.n2engine.player.N2Activity"

adb shell am force-stop "$app"
adb shell am start -W -n "$activity" > /dev/null
sleep 12
before="$(adb shell pidof "$app" | tr -d '\r')"
test -n "$before" || { echo "::error::$app did not start"; exit 1; }
adb logcat -d --pid="$before" | grep -E "N2Engine Android player: the (Vulkan|software) renderer" | tail -1 || true

adb shell input keyevent KEYCODE_HOME
sleep 6
adb logcat -c
adb shell am start -W -n "$activity" > /dev/null
sleep 14
after="$(adb shell pidof "$app" | tr -d '\r')"
if [ "$after" != "$before" ]; then
  echo "::error::$app was not resumed: process $before before, '${after:-none}' after"
  exit 1
fi
frames="$(adb logcat -d --pid="$after" | grep -E "N2Engine Android player: [0-9]+ frames" || true)"
echo "$frames" | tail -3
if [ -z "$frames" ]; then
  echo "::error::$app logged no frames after coming back from the background"
  adb logcat -d --pid="$after" | tail -40
  exit 1
fi
echo "Resumed in the same process ($after), drawing again"
