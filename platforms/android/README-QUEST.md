Warzone 2100 VR (Meta Quest / PICO)
===================================

A standalone VR port of Warzone 2100 for Meta Quest 2/3/3S/Pro and PICO 4 headsets
(arm64 Android, OpenXR, Vulkan). The battlefield is a diorama on a table in front of
you, the menus and the HUD sit on a floating panel, and you play with a laser pointer.

Install
-------
1. Enable developer mode on the headset and connect it by USB.
2. adb install -r Warzone2100-VR-Quest-v<version>.apk
3. Start "Warzone 2100" from the library (Unknown sources). The first start unpacks
   the game data (about a minute).

The APK is the full game, including the campaign videos.

Controls
--------
  right controller laser   cursor (panel and table)
  right trigger            left click (select, drag a box, buttons)
  right grip               right click (orders)
  left trigger             Shift (add to selection, queue orders)
  left grip                Ctrl (assign groups)
  left stick               pan the camera
  right stick              turn (left/right), zoom (up/down), scroll lists
  A                        Enter
  B, left menu button      Escape (back, game menu)
  X                        stop / hold position
  Y                        back (research, intelligence)
  short left stick click   mixed reality on/off (passthrough, your room around the table)
  hold both stick clicks   recenter the table and the panel

Text fields open a virtual keyboard; type on it with the laser and the right trigger.

Build
-----
Requires the Android SDK, NDK 27 and vcpkg (arm64-android).

  cd platforms/android
  ./build-native.sh            # native library + game data (+ campaign videos)
  ./gradlew assembleRelease    # app/build/outputs/apk/release/app-release.apk

WZ_NO_SEQUENCES=1 ./build-native.sh skips the campaign videos for quicker test builds.

Code: lib/sdl/wz_openxr*.cpp (OpenXR presenter, laser, passthrough, controllers),
src/vr_diorama.cpp (table view, sky), src/vr_keyboard.cpp (virtual keyboard).
