cmake_minimum_required(VERSION 3.15)
project(uhdr_repack LANGUAGES CXX OBJCXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_STANDARD 17)

if(APPLE)
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
  # Pin release tag (matches lib major.minor.patch in upstream CMakeLists).
  set(_LIBUHDR_TAG "v1.4.0")
  FetchContent_Declare(
    libultrahdr
    GIT_REPOSITORY https://github.com/google/libultrahdr.git
    GIT_TAG ${_LIBUHDR_TAG}
    GIT_SHALLOW TRUE
  )
  # Must be set before add_subdirectory so libultrahdr's option_if_not_defined sees them.
  set(UHDR_WRITE_XMP ON CACHE BOOL "Write gainmap metadata in XMP packet" FORCE)
  set(UHDR_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(UHDR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(UHDR_BUILD_BENCHMARK OFF CACHE BOOL "" FORCE)
  set(UHDR_BUILD_FUZZERS OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(libultrahdr)
endif()

find_package(JPEG REQUIRED)

if(NOT APPLE)
  message(FATAL_ERROR "uhdr_repack requires macOS 26 (Tahoe), ARM64 for HDR TIFF ingest via Core Image.")
endif()

find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)
find_library(CORE_IMAGE_FRAMEWORK CoreImage REQUIRED)
find_library(CORE_GRAPHICS_FRAMEWORK CoreGraphics REQUIRED)

add_executable(uhdr_repack
  src/main.cpp
  src/uhdr_encode.cpp
  src/gainmap_metadata.cpp
  src/verify.cpp
  src/tiff_input.mm
  src/sdr_input.mm
)

target_include_directories(uhdr_repack PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

if(UHDR_USE_SYSTEM)
  target_include_directories(uhdr_repack PRIVATE "${_uhdr_vendor_include}")
  target_link_libraries(uhdr_repack PRIVATE "${_uhdr_vendor_lib}")
else()
  # Vendored uhdr target does not always propagate public headers to dependents (seen on clean CI).
  # ultrahdr_api.h lives at the libultrahdr tree root (EXPORT_INCLUDE_DIR upstream).
  target_include_directories(uhdr_repack PRIVATE "${libultrahdr_SOURCE_DIR}")
  target_link_libraries(uhdr_repack PRIVATE uhdr)
endif()

target_link_libraries(uhdr_repack PRIVATE
  JPEG::JPEG
  ${FOUNDATION_FRAMEWORK}
  ${CORE_IMAGE_FRAMEWORK}
  ${CORE_GRAPHICS_FRAMEWORK}
)

install(TARGETS uhdr_repack RUNTIME DESTINATION bin)
