#!/usr/bin/env bash
# Build only the native endpoint in the web job: no engine, renderer, physics or downloaded native libraries.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
out="${1:?usage: build-websocket-native-smoke.sh <output-directory>}"
mkdir -p "$out"
out="$(cd "$out" && pwd)"
for module in sha1 base64 platform_util; do
  "${CC:-cc}" -O2 -I"$root/external/mbedtls/include" -c \
    "$root/external/mbedtls/library/$module.c" -o "$out/$module.o"
done
"${CXX:-c++}" -std=c++23 -O2 -I"$root/engine/include" -I"$root/external/nlohmann/include" \
  -I"$root/external/mbedtls/include" "$root/tests/n2server/WebSocketEcho.cpp" \
  "$root/engine/src/network/n2net/WebSocketTransport.cpp" \
  "$out/sha1.o" "$out/base64.o" "$out/platform_util.o" -pthread -o "$out/websocket_echo"
