if(NOT (CMAKE_SYSTEM_NAME MATCHES "Windows" OR CMAKE_SYSTEM_NAME MATCHES "Darwin"))
	# Temporary workaround for systems with broken SDL2 CMake config (ex. imported target does not exist in "SDL2Targets.cmake")
	# Can be manually set to "ON" to avoid using the CONFIG mode
	OPTION(WZ_FINDSDL2_NOCONFIG "Disable trying to find SDL2 using CONFIG mode" OFF)
endif()

file(GLOB HEADERS "*.h")
file(GLOB SRC "*.cpp")

# ControllerImage provides per-device controller button glyph rendering (vendored, single C file, requires only SDL3)
set(CONTROLLERIMAGE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../3rdparty/ControllerImage/src")
list(APPEND SRC "${CONTROLLERIMAGE_DIR}/controllerimage.c")
if(MSVC)
	set_source_files_properties("${CONTROLLERIMAGE_DIR}/controllerimage.c" PROPERTIES COMPILE_OPTIONS "/w")
else()
	set_source_files_properties("${CONTROLLERIMAGE_DIR}/controllerimage.c" PROPERTIES COMPILE_OPTIONS "-w")
endif()

if(CMAKE_SYSTEM_NAME MATCHES "Darwin")
	list(APPEND SRC "cocoa_sdl_helpers.mm" "cocoa_wz_menus.mm")
endif()

# Find SDL3
find_package(SDL3 3.2.12 REQUIRED CONFIG REQUIRED COMPONENTS SDL3)

add_library(sdl-backend STATIC ${HEADERS} ${SRC})
include(WZTargetConfiguration)
WZ_TARGET_CONFIGURATION(sdl-backend)

target_link_libraries(sdl-backend PRIVATE framework ivis-opengl optional-lite)
set_property(TARGET sdl-backend PROPERTY FOLDER "lib")

target_include_directories(sdl-backend PRIVATE "${CONTROLLERIMAGE_DIR}")

if(ANDROID)
	# Standalone VR headsets (Meta Quest): render through OpenXR
	OPTION(WZ_ENABLE_OPENXR "Present through OpenXR (XR_KHR_vulkan_enable2)" ON)
else()
	OPTION(WZ_ENABLE_OPENXR "Present through OpenXR (XR_KHR_vulkan_enable2)" OFF)
endif()
if(WZ_ENABLE_OPENXR)
	# OPENXR_SDK_DIR must contain include/openxr/openxr.h and the loader library (lib/libopenxr_loader.so)
	set(OPENXR_SDK_DIR "" CACHE PATH "OpenXR headers + loader (see platforms/android/build-native.sh)")
	find_path(OPENXR_INCLUDE_DIR openxr/openxr.h PATHS "${OPENXR_SDK_DIR}/include" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
	find_library(OPENXR_LOADER_LIBRARY openxr_loader PATHS "${OPENXR_SDK_DIR}/lib" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
	if(NOT OPENXR_INCLUDE_DIR OR NOT OPENXR_LOADER_LIBRARY)
		message(FATAL_ERROR "WZ_ENABLE_OPENXR: OpenXR SDK not found in OPENXR_SDK_DIR (${OPENXR_SDK_DIR})")
	endif()
	message(STATUS "OpenXR enabled: ${OPENXR_LOADER_LIBRARY}")
	target_include_directories(sdl-backend PRIVATE "${OPENXR_INCLUDE_DIR}")
	target_link_libraries(sdl-backend PRIVATE "${OPENXR_LOADER_LIBRARY}")
	target_compile_definitions(sdl-backend PRIVATE "WZ_OPENXR_ENABLED")
endif()

message( STATUS "Linking to SDL3 library" )
target_link_libraries(sdl-backend PRIVATE SDL3::SDL3)
target_compile_definitions(sdl-backend PRIVATE "-DHAVE_SDL_VULKAN_H")

if (CMAKE_CXX_STANDARD GREATER_EQUAL 20)
	# Workaround for Apple requiring min macOS version 13.3 when C++20 is used.
	# Temporarily require C++17 for sdl-backend for all platforms, for the sake
	# of uniformity.
	set_target_properties(sdl-backend PROPERTIES CXX_STANDARD 17)
endif()
