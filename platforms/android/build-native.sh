#!/usr/bin/env bash
# Builds Warzone 2100 for Meta Quest (Android arm64) and stages the results
# for Gradle:
#   app/src/main/jniLibs/arm64-v8a/libmain.so   game + statically linked deps
#   app/src/main/assets/data/                   base.wz, mp.wz, fonts, music, ...
#   app/src/sdl/java/                           SDL3's Java glue (version-matched)
#
# Requirements: Android SDK/NDK, vcpkg (VCPKG_ROOT), host autotools.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$(cd "$HERE/../.." && pwd)"
OUT="${WZ_ANDROID_BUILD_DIR:-$SRC/build/android}"

ANDROID_HOME="${ANDROID_HOME:-$HOME/Android/Sdk}"
ANDROID_NDK_HOME="${ANDROID_NDK_HOME:-$ANDROID_HOME/ndk/27.2.12479018}"
VCPKG_ROOT="${VCPKG_ROOT:-$HOME/vcpkg}"
BUILD_TYPE="${BUILD_TYPE:-RelWithDebInfo}"
API_LEVEL=32
export ANDROID_NDK_HOME VCPKG_ROOT

# OpenXR loader + headers, from the official Khronos Android AAR (Gradle packages the same AAR into the APK)
OPENXR_VERSION=1.1.62
OPENXR_DIR="$OUT/openxr"
if [ ! -f "$OPENXR_DIR/sdk/lib/libopenxr_loader.so" ]; then
	mkdir -p "$OPENXR_DIR"
	AAR="$OPENXR_DIR/openxr_loader_for_android-$OPENXR_VERSION.aar"
	[ -f "$AAR" ] || curl -sSfL -o "$AAR" "https://repo1.maven.org/maven2/org/khronos/openxr/openxr_loader_for_android/$OPENXR_VERSION/openxr_loader_for_android-$OPENXR_VERSION.aar"
	rm -rf "$OPENXR_DIR/aar" "$OPENXR_DIR/sdk"
	unzip -q "$AAR" -d "$OPENXR_DIR/aar"
	mkdir -p "$OPENXR_DIR/sdk/lib"
	cp -r "$OPENXR_DIR/aar/prefab/modules/headers/include" "$OPENXR_DIR/sdk/include"
	cp "$OPENXR_DIR/aar/jni/arm64-v8a/libopenxr_loader.so" "$OPENXR_DIR/sdk/lib/"
fi

cmake -S "$SRC" -B "$OUT/native" -G Ninja \
	-DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
	-DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
	-DVCPKG_CHAINLOAD_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
	-DVCPKG_TARGET_TRIPLET=arm64-android \
	-DVCPKG_HOST_TRIPLET=x64-linux \
	-DVCPKG_INSTALLED_DIR="$OUT/vcpkg_installed" \
	-DVCPKG_OVERLAY_PORTS="$SRC/.ci/vcpkg/overlay-ports" \
	-DVCPKG_OVERLAY_TRIPLETS="$SRC/.ci/vcpkg/overlay-triplets" \
	-DANDROID_ABI=arm64-v8a \
	-DANDROID_PLATFORM="android-$API_LEVEL" \
	-DANDROID_STL=c++_static \
	-DGLSLC="$ANDROID_NDK_HOME/shader-tools/linux-x86_64/glslc" \
	-DCMAKE_INSTALL_PREFIX="$OUT/install" \
	-DWZ_ENABLE_WARNINGS_AS_ERRORS=OFF \
	-DWZ_DISTRIBUTOR="Quest port" \
	-DWZ_ENABLE_OPENXR=ON \
	-DENABLE_GNS_NETWORK_BACKEND=OFF \
	-DOPENXR_SDK_DIR="$OPENXR_DIR/sdk" \
	"$@"

cmake --build "$OUT/native"
cmake --install "$OUT/native" --component Core
cmake --install "$OUT/native" --component Data 2>/dev/null || true

# Stage native library
mkdir -p "$HERE/app/src/main/jniLibs/arm64-v8a"
# Strip the packaged copy; the unstripped build/android/native/src/libmain.so keeps symbols for ndk-stack
"$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip" --strip-debug \
	-o "$HERE/app/src/main/jniLibs/arm64-v8a/libmain.so" "$OUT/native/src/libmain.so"

# Stage game data (whatever the install put into the data dir)
DATA_INSTALL="$(find "$OUT/install" -type f -name base.wz -printf '%h\n' | head -n1)"
if [ -z "$DATA_INSTALL" ]; then
	echo "base.wz not found under $OUT/install" >&2
	exit 1
fi
rm -rf "$HERE/app/src/main/assets/data"
mkdir -p "$HERE/app/src/main/assets"
cp -r "$DATA_INSTALL" "$HERE/app/src/main/assets/data"
# Campaign videos (full game APK). WZ_NO_SEQUENCES=1 skips them for quicker test builds.
SEQUENCES_URL="https://github.com/Warzone2100/wz-sequences/releases/download/v3/standard-quality-en-sequences.wz"
SEQUENCES_WZ="$OUT/sequences.wz"
if [ "${WZ_NO_SEQUENCES:-0}" != "1" ]; then
	if [ ! -f "$SEQUENCES_WZ" ]; then
		echo "Downloading the campaign videos ($SEQUENCES_URL)"
		curl -sSfL -o "$SEQUENCES_WZ.part" "$SEQUENCES_URL" && mv "$SEQUENCES_WZ.part" "$SEQUENCES_WZ"
	fi
	cp "$SEQUENCES_WZ" "$HERE/app/src/main/assets/data/sequences.wz"
fi
# Content hash: the app re-extracts its data only when this changes
(cd "$HERE/app/src/main/assets/data" && find . -type f -print0 | sort -z | xargs -0 sha1sum | sha1sum | cut -c1-40) \
	> "$HERE/app/src/main/assets/data.version"

# Stage SDL3's Java glue from the exact SDL3 source vcpkg built
# (SDL refuses to start when the Java glue and native library versions differ)
SDL_H="$OUT/vcpkg_installed/arm64-android/include/SDL3/SDL_version.h"
SDL_VER="$(awk '/define SDL_(MAJOR|MINOR|MICRO)_VERSION/ {printf "%s%s", sep, $3; sep="."}' "$SDL_H")"
SDL_JAVA="$(find "$VCPKG_ROOT/buildtrees/sdl3/src" -maxdepth 6 -type d -path "*-$SDL_VER-*/android-project/app/src/main/java" | head -n1)"
if [ -z "$SDL_JAVA" ]; then
	echo "SDL3 $SDL_VER Java sources not found in vcpkg buildtrees" >&2
	exit 1
fi
echo "Using SDL $SDL_VER Java glue: $SDL_JAVA"
rm -rf "$HERE/app/src/sdl/java"
mkdir -p "$HERE/app/src/sdl"
cp -r "$SDL_JAVA" "$HERE/app/src/sdl/java"

echo "Native build staged. Now run: (cd $HERE && ./gradlew assembleDebug)"
