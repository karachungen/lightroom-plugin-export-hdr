cmake_minimum_required(VERSION 3.23)

if(APPLE)
  project(uhdr_repack LANGUAGES CXX OBJCXX)
else()
  project(uhdr_repack LANGUAGES CXX)
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
option(UHDR_ENABLE_GUI "Build Qt 6.11 preview/edit UI (required for --edit)" ON)
option(UHDR_STATIC_QT "Link Qt statically into uhdr_repack (no .dylib/.dll)" ON)

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
  set(_LIBUHDR_TAG "v1.4.0")
  FetchContent_Declare(
    libultrahdr
    GIT_REPOSITORY https://github.com/google/libultrahdr.git
    GIT_TAG ${_LIBUHDR_TAG}
    GIT_SHALLOW TRUE
    PATCH_COMMAND ${CMAKE_COMMAND}
      "-DJPEG_ENCODER_CPP=<SOURCE_DIR>/lib/src/jpegencoderhelper.cpp"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/patches/apply_libultrahdr_progressive.cmake"
  )
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build libultrahdr as a static library" FORCE)
  set(UHDR_WRITE_XMP ON CACHE BOOL "Write gainmap metadata in XMP packet" FORCE)
  set(UHDR_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(UHDR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(UHDR_BUILD_BENCHMARK OFF CACHE BOOL "" FORCE)
  set(UHDR_BUILD_FUZZERS OFF CACHE BOOL "" FORCE)
  set(UHDR_MAX_DIMENSION 65500 CACHE STRING "Maximum input width and height supported by libultrahdr" FORCE)
  # Vendor + statically build libjpeg-turbo instead of linking whatever the host has installed
  # (e.g. Homebrew's dylib, which pins an absolute /opt/homebrew or /usr/local path).
  set(UHDR_BUILD_DEPS ON CACHE BOOL "Build libjpeg-turbo via libultrahdr" FORCE)
  FetchContent_MakeAvailable(libultrahdr)
  # Re-apply if the source tree was already populated without PATCH_COMMAND.
  execute_process(
    COMMAND ${CMAKE_COMMAND}
      "-DJPEG_ENCODER_CPP=${libultrahdr_SOURCE_DIR}/lib/src/jpegencoderhelper.cpp"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/patches/apply_libultrahdr_progressive.cmake"
    COMMAND_ERROR_IS_FATAL ANY
  )
endif()

include(FetchContent)
FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

set(_UHDR_CORE_SOURCES
  src/main.cpp
  src/cli_encode.cpp
  src/cli_inspect.cpp
  src/encode_engine.cpp
  src/session.cpp
  src/activity_log.cpp
  src/hdr_bridge.cpp
  src/slice_plan.cpp
  src/resize_lanczos.cpp
  src/uhdr_encode.cpp
  src/jpeg_xmp_rights.cpp
  src/gainmap_metadata.cpp
  src/gainmap_loader.cpp
  src/gainmap_edit.cpp
  src/verify.cpp
  src/half_float.cpp
  src/yuv_convert.cpp
)

set(_UHDR_GUI_SOURCES
  src/gui/gui_app.cpp
  src/gui/gui_self_test.cpp
  src/gui/hdr_rhi_viewport.cpp
  src/gui/preview_document.cpp
  src/gui/ultrahdr_preview_service.cpp
  src/gui/gainmap_canvas.cpp
  src/gui/main_window.cpp
  src/gui/gui_bridge.cpp
  src/gui/hdr_tiff_client.cpp
  src/gui/web_resources.cpp
)

if(APPLE)
  list(APPEND _UHDR_CORE_SOURCES
    src/gainmap_compute.mm
    src/tiff_input.mm
    src/sdr_input.mm
  )
elseif(WIN32)
  list(APPEND _UHDR_CORE_SOURCES
    src/gainmap_compute_win.cpp
    src/tiff_input_win.cpp
    src/sdr_input_win.cpp
    src/wic_utils.cpp
  )
else()
  message(FATAL_ERROR "uhdr_repack supports macOS (Core Image) and Windows (WIC) only.")
endif()

set(_UHDR_ALL_SOURCES ${_UHDR_CORE_SOURCES})

if(UHDR_ENABLE_GUI)
  set(CMAKE_AUTOMOC ON)
  set(CMAKE_AUTORCC ON)

  set(_UHDR_GUI_HEADERS
    include/gui/gainmap_canvas.h
    include/gui/gui_bridge.h
    include/gui/hdr_tiff_client.h
    include/gui/hdr_rhi_viewport.h
    include/gui/preview_document.h
    include/gui/ultrahdr_preview_service.h
    include/gui/web_chrome.h
    include/gui/main_window.h
  )

  list(APPEND _UHDR_ALL_SOURCES ${_UHDR_GUI_HEADERS})

  if(DEFINED ENV{QT_STATIC_ROOT})
    list(APPEND CMAKE_PREFIX_PATH "$ENV{QT_STATIC_ROOT}")
  endif()
  if(DEFINED QT_STATIC_ROOT)
    list(APPEND CMAKE_PREFIX_PATH "${QT_STATIC_ROOT}")
  endif()

  if(UHDR_STATIC_QT)
    set(QT_USE_STATIC_LIBS ON)
    set(CMAKE_FIND_PACKAGE_PREFER_CONFIG ON)
  endif()

  find_package(Qt6 6.11 REQUIRED COMPONENTS Core Concurrent Gui GuiPrivate Widgets ShaderTools)

  if(APPLE)
    list(APPEND _UHDR_GUI_SOURCES
      src/gui/web_chrome_mac.mm
      src/gui/app_icon_mac.mm
    )
  elseif(WIN32)
    list(APPEND _UHDR_GUI_SOURCES src/gui/web_chrome_win.cpp)
  endif()

  list(APPEND _UHDR_ALL_SOURCES ${_UHDR_GUI_SOURCES})
endif()

add_executable(uhdr_repack ${_UHDR_ALL_SOURCES})

if(WIN32)
  set(_uhdr_rc "${CMAKE_CURRENT_SOURCE_DIR}/resources/uhdr_repack.rc")
  set_source_files_properties("${_uhdr_rc}" PROPERTIES
    OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/resources/app.ico"
  )
  target_sources(uhdr_repack PRIVATE "${_uhdr_rc}")
endif()


target_include_directories(uhdr_repack PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

if(UHDR_USE_SYSTEM)
  target_include_directories(uhdr_repack PRIVATE "${_uhdr_vendor_include}")
  target_link_libraries(uhdr_repack PRIVATE "${_uhdr_vendor_lib}")
else()
  target_include_directories(uhdr_repack PRIVATE "${libultrahdr_SOURCE_DIR}")
  target_link_libraries(uhdr_repack PRIVATE uhdr)
endif()

target_link_libraries(uhdr_repack PRIVATE nlohmann_json::nlohmann_json)

if(UHDR_ENABLE_GUI)
  target_compile_definitions(uhdr_repack PRIVATE UHDR_ENABLE_GUI=1)
  target_link_libraries(uhdr_repack PRIVATE
    Qt6::Core
    Qt6::Concurrent
    Qt6::Gui
    Qt6::GuiPrivate
    Qt6::Widgets
  )
  if(APPLE)
    find_library(APPKIT_FRAMEWORK AppKit REQUIRED)
    find_library(WEBKIT_FRAMEWORK WebKit REQUIRED)
    target_link_libraries(uhdr_repack PRIVATE ${APPKIT_FRAMEWORK} ${WEBKIT_FRAMEWORK})
  endif()
  if(WIN32)
    set(_WEBVIEW2_SDK "$ENV{WEBVIEW2_SDK}")
    if(NOT _WEBVIEW2_SDK)
      set(_WEBVIEW2_SDK "C:/Program Files (x86)/Microsoft Edge WebView2")
    endif()
    if(EXISTS "${_WEBVIEW2_SDK}/include/WebView2.h")
      target_include_directories(uhdr_repack PRIVATE "${_WEBVIEW2_SDK}/include")
      find_library(WEBVIEW2_LOADER WebView2LoaderStatic PATHS
        "${_WEBVIEW2_SDK}/lib/x64"
        "${_WEBVIEW2_SDK}/lib/x86"
        NO_DEFAULT_PATH)
      if(WEBVIEW2_LOADER)
        target_link_libraries(uhdr_repack PRIVATE ${WEBVIEW2_LOADER})
      else()
        message(WARNING "WebView2LoaderStatic not found; install WebView2 SDK for GUI builds.")
      endif()
    else()
      message(WARNING "WebView2 SDK not found at ${_WEBVIEW2_SDK}; install Evergreen WebView2 SDK.")
    endif()
  endif()
endif()

if(UHDR_ENABLE_GUI)
  qt6_add_shaders(uhdr_repack "uhdr_preview_shaders"
    PREFIX
      "/hdr"
    FILES
      shaders/hdr_preview.vert
      shaders/hdr_preview.frag
  )
  qt_add_resources(uhdr_repack "uhdr_web_resources"
    PREFIX /
    BASE "${CMAKE_CURRENT_SOURCE_DIR}"
    FILES
      web/index.html
      web/app.css
      web/app.js
      web/fonts/fraunces.woff2
      web/fonts/barlow-condensed.woff2
      resources/app.png
  )
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
  if(UHDR_STATIC_QT AND UHDR_ENABLE_GUI)
    target_link_options(uhdr_repack PRIVATE -static-libstdc++)
  endif()
elseif(WIN32)
  target_link_libraries(uhdr_repack PRIVATE windowscodecs ole32)
  if(UHDR_STATIC_QT AND UHDR_ENABLE_GUI)
    target_compile_options(uhdr_repack PRIVATE /MT)
  endif()
endif()

if(UHDR_STATIC_QT AND UHDR_ENABLE_GUI)
  add_custom_command(TARGET uhdr_repack POST_BUILD
    COMMAND ${CMAKE_COMMAND}
      -DUHDR_BINARY=$<TARGET_FILE:uhdr_repack>
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyStaticQt.cmake"
    COMMENT "Verifying the single executable has no shared Qt dependencies"
  )
endif()

install(TARGETS uhdr_repack RUNTIME DESTINATION bin)
