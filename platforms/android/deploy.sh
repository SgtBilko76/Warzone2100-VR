#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
PKG="net.wz2100.warzone2100.quest"

if [ -z "${SKIP_NATIVE:-}" ]; then
    "$HERE/build-native.sh"
fi
[ -f "$HERE/local.properties" ] || echo "sdk.dir=${ANDROID_HOME:-$HOME/Android/Sdk}" > "$HERE/local.properties"
(cd "$HERE" && ./gradlew assembleDebug -q)

# Fixed path to the generated APK
APK="$HERE/app/build/outputs/apk/debug/app-debug.apk"
echo "Installing $APK..."
adb install -r "$APK"
adb shell am force-stop "$PKG" || true
adb logcat -c
adb shell am start -n "$PKG/net.wz2100.warzone2100.WarzoneActivity"
echo ""
echo "Logs: adb logcat -s WZ2100:* SDL:* OpenXR:* DEBUG:*"
