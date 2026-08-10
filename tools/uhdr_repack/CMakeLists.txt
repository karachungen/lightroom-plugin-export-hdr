cmake_minimum_required(VERSION 3.15)

if(POLICY CMP0091)
  cmake_policy(SET CMP0091 NEW)
endif()

if(APPLE)
  project(uhdr_repack LANGUAGES CXX OBJCXX)
else()
  project(uhdr_repack LANGUAGES CXX)
endif()

if(MSVC)
  # Match the statically linked upstream libuhdr and keep the plug-in binary self-contained.
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(APPLE)
  set(CMAKE_OBJCXX_STANDARD 17)
  if(NOT DEFINED CMAKE_OSX_SYSROOT OR CMAKE_OSX_SYSROOT STREQUAL "")
    set(_uhdr_macos_sdk "")
    execute_process(
      COMMAND xcrun --show-sdk-path
      OUTPUT_VARIABLE _uhdr_macos_sdk
      OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(_uhdr_macos_sdk)
      set(CMAKE_OSX_SYSROOT "${_uhdr_macos_sdk}" CACHE PATH "macOS SDK for frameworks" FORCE)
    else()
      set(_uhdr_sdk_fallback "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk")
      if(EXISTS "${_uhdr_sdk_fallback}")
        set(CMAKE_OSX_SYSROOT "${_uhdr_sdk_fallback}" CACHE PATH "macOS SDK for frameworks" FORCE)
      endif()
    endif()
  endif()
endif()

option(UHDR_USE_SYSTEM "Link against a preinstalled libultrahdr instead of the vendored FetchContent build" OFF)
option(UHDR_ROOT "When UHDR_USE_SYSTEM is ON: prefix where libultrahdr is installed (include/, lib/)" "")

set(_uhdr_vendor_include "")
set(_uhdr_vendor_lib "")

if(UHDR_USE_SYSTEM)
  if(UHDR_ROOT)
    list(APPEND CMAKE_PREFIX_PATH "${UHDR_ROOT}")
  endif()
  find_path(UHDR_INCLUDE_DIR NAMES ultrahdr_api.h
    PATHS
      "${UHDR_ROOT}/include"
      /usr/local/include
      /opt/homebrew/include
  )
  find_library(UHDR_LIBRARY NAMES uhdr libuhdr
    PATHS
      "${UHDR_ROOT}/lib"
      /usr/local/lib
      /opt/homebrew/lib
  )
  if(NOT UHDR_INCLUDE_DIR OR NOT UHDR_LIBRARY)
    message(FATAL_ERROR
      "libultrahdr not found. Install it or leave UHDR_USE_SYSTEM=OFF to build the vendored copy. "
      "Pass -DUHDR_ROOT=/path/to/prefix if needed.")
  endif()
  set(_uhdr_vendor_include "${UHDR_INCLUDE_DIR}")
  set(_uhdr_vendor_lib "${UHDR_LIBRARY}")
else()
  include(FetchContent)
  # google/libultrahdr PR #394: JFIF first after SOI and MPF immediately before SOS.
  # Pin the reviewed upstream merge commit so the build remains reproducible.
  set(_LIBUHDR_TAG "11ac0c325bbf56ecf8be8704ff0f79fc9e1aac77")
  FetchContent_Declare(
    libultrahdr
    GIT_REPOSITORY https://github.com/google/libultrahdr.git
    GIT_TAG ${_LIBUHDR_TAG}
  )
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build libultrahdr as a static library" FORCE)
  set(UHDR_WRITE_XMP ON CACHE BOOL "Write gainmap metadata in XMP packet" FORCE)
  set(UHDR_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(UHDR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(UHDR_BUILD_BENCHMARK OFF CACHE BOOL "" FORCE)
  set(UHDR_BUILD_FUZZERS OFF CACHE BOOL "" FORCE)
  # Vendor + statically build libjpeg-turbo instead of linking whatever the host has installed
  # (e.g. Homebrew's dylib, which pins an absolute /opt/homebrew or /usr/local path).
  set(UHDR_BUILD_DEPS ON CACHE BOOL "Build libjpeg-turbo via libultrahdr" FORCE)
  FetchContent_MakeAvailable(libultrahdr)
endif()

set(_UHDR_COMMON_SOURCES
  src/main.cpp
  src/slice_plan.cpp
  src/uhdr_encode.cpp
  src/compatibility_gainmap.cpp
  src/gainmap_metadata.cpp
  src/verify.cpp
  src/half_float.cpp
  src/yuv_convert.cpp
)

if(APPLE)
  list(APPEND _UHDR_COMMON_SOURCES
    src/tiff_input.mm
    src/sdr_input.mm
  )
elseif(WIN32)
  list(APPEND _UHDR_COMMON_SOURCES
    src/tiff_input_win.cpp
    src/sdr_input_win.cpp
    src/wic_utils.cpp
  )
else()
  message(FATAL_ERROR "uhdr_repack supports macOS (Core Image) and Windows (WIC) only.")
endif()

add_executable(uhdr_repack ${_UHDR_COMMON_SOURCES})

target_include_directories(uhdr_repack PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

if(UHDR_USE_SYSTEM)
  target_include_directories(uhdr_repack PRIVATE "${_uhdr_vendor_include}")
  target_link_libraries(uhdr_repack PRIVATE "${_uhdr_vendor_lib}")
  find_package(JPEG REQUIRED)
  target_link_libraries(uhdr_repack PRIVATE JPEG::JPEG)
else()
  target_include_directories(uhdr_repack PRIVATE "${libultrahdr_SOURCE_DIR}")
  target_include_directories(uhdr_repack PRIVATE
    "${libultrahdr_SOURCE_DIR}/third_party/turbojpeg/src"
    "${libultrahdr_BINARY_DIR}/turbojpeg/src/turbojpeg-build"
  )
  # Current libultrahdr names its static MSVC target uhdr-static; v1.4.0 used uhdr.
  if(TARGET uhdr-static)
    target_link_libraries(uhdr_repack PRIVATE uhdr-static)
  else()
    target_link_libraries(uhdr_repack PRIVATE uhdr)
  endif()
endif()

if(APPLE)
  find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)
  find_library(CORE_IMAGE_FRAMEWORK CoreImage REQUIRED)
  find_library(CORE_GRAPHICS_FRAMEWORK CoreGraphics REQUIRED)
  target_link_libraries(uhdr_repack PRIVATE
    ${FOUNDATION_FRAMEWORK}
    ${CORE_IMAGE_FRAMEWORK}
    ${CORE_GRAPHICS_FRAMEWORK}
  )
elseif(WIN32)
  target_link_libraries(uhdr_repack PRIVATE windowscodecs ole32)
endif()

install(TARGETS uhdr_repack RUNTIME DESTINATION bin)

include(CTest)
if(BUILD_TESTING)
  add_executable(compatibility_gainmap_test
    test/compatibility_gainmap_test.cpp
    src/compatibility_gainmap.cpp
    src/half_float.cpp
  )
  target_include_directories(compatibility_gainmap_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
  )
  if(UHDR_USE_SYSTEM)
    target_include_directories(compatibility_gainmap_test PRIVATE "${_uhdr_vendor_include}")
    target_link_libraries(compatibility_gainmap_test PRIVATE "${_uhdr_vendor_lib}" JPEG::JPEG)
  else()
    target_include_directories(compatibility_gainmap_test PRIVATE
      "${libultrahdr_SOURCE_DIR}"
      "${libultrahdr_SOURCE_DIR}/third_party/turbojpeg/src"
      "${libultrahdr_BINARY_DIR}/turbojpeg/src/turbojpeg-build"
    )
    if(TARGET uhdr-static)
      target_link_libraries(compatibility_gainmap_test PRIVATE uhdr-static)
    else()
      target_link_libraries(compatibility_gainmap_test PRIVATE uhdr)
    endif()
  endif()
  add_test(NAME compatibility_gainmap_test COMMAND compatibility_gainmap_test)
endif()
