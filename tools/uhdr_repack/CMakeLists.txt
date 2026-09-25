cmake_minimum_required(VERSION 3.23)

if(APPLE)
  project(uhdr_repack LANGUAGES C CXX OBJCXX)
else()
  project(uhdr_repack LANGUAGES C CXX)
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
  set(_LIBUHDR_TAG "v2.0.2")
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
  # The plug-in writes JPEG gain maps. HEIF/AVIF is a separate libheif build this binary does not ship.
  set(UHDR_ENABLE_HEIF OFF CACHE BOOL "Build libultrahdr without HEIF/AVIF" FORCE)
  # Vendor + statically build libjpeg-turbo instead of linking whatever the host has installed
  # (e.g. Homebrew's dylib, which pins an absolute /opt/homebrew or /usr/local path).
  set(UHDR_BUILD_DEPS ON CACHE BOOL "Build libjpeg-turbo via libultrahdr" FORCE)
  # v2 static MSVC builds switch the CRT to /MT. Shared Qt is /MD, so keep the
  # parent runtime. libultrahdr treats BUILD_FOR_WINUI as "do not force /MT".
  if(WIN32 AND NOT UHDR_STATIC_QT)
    set(BUILD_FOR_WINUI ON CACHE BOOL "Keep libultrahdr on the DLL CRT for shared Qt" FORCE)
  endif()
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
  GIT_TAG v3.12.0
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

FetchContent_Declare(
  miniz
  URL https://github.com/richgel999/miniz/releases/download/3.0.2/miniz-3.0.2.zip
  URL_HASH SHA256=ada38db0b703a56d3dd6d57bf84a9c5d664921d870d8fea4db153979fb5332c5
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SOURCE_SUBDIR no-cmake-project
)
FetchContent_MakeAvailable(miniz)

set(_UHDR_CORE_SOURCES
  src/main.cpp
  src/cli_encode.cpp
  src/encode_presets.cpp
  src/cli_inspect.cpp
  src/encode_engine.cpp
  src/session.cpp
  src/activity_log.cpp
  src/hdr_bridge.cpp
  src/slice_plan.cpp
  src/resize_lanczos.cpp
  src/uhdr_encode.cpp
  src/jpeg_container.cpp
  src/jpeg_xmp_rights.cpp
  src/gainmap_metadata.cpp
  src/gainmap_loader.cpp
  src/verify.cpp
  src/half_float.cpp
  src/tiff_float.cpp
  src/icc_profile.cpp
  src/hdr_chart.cpp
  src/chart_io.cpp
  src/hdr_scenes.cpp
  src/yuv_convert.cpp
  src/sdr_jpeg.cpp
  src/cli_describe.cpp
  src/delivery_check.cpp
  src/instagram_sim.cpp
  "${miniz_SOURCE_DIR}/miniz.c"
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
  src/gui/check_preview_jpeg.cpp
  src/gainmap_edit.cpp
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

  set(_uhdr_static_qt "")
  if(DEFINED QT_STATIC_ROOT AND NOT "${QT_STATIC_ROOT}" STREQUAL "")
    set(_uhdr_static_qt "${QT_STATIC_ROOT}")
  elseif(DEFINED ENV{QT_STATIC_ROOT} AND NOT "$ENV{QT_STATIC_ROOT}" STREQUAL "")
    set(_uhdr_static_qt "$ENV{QT_STATIC_ROOT}")
  endif()

  if(UHDR_STATIC_QT)
    set(QT_USE_STATIC_LIBS ON)
    set(CMAKE_FIND_PACKAGE_PREFER_CONFIG ON)
    if(_uhdr_static_qt)
      list(PREPEND CMAKE_PREFIX_PATH "${_uhdr_static_qt}")
      # A previous shared-Qt configure caches Qt6_DIR under Homebrew. find_package
      # will not look at the static kit until those entries are cleared.
      if(DEFINED Qt6_DIR AND NOT Qt6_DIR MATCHES "^${_uhdr_static_qt}/")
        message(STATUS "Dropping cached Qt at ${Qt6_DIR}; using ${_uhdr_static_qt}")
        get_cmake_property(_uhdr_cache_vars CACHE_VARIABLES)
        foreach(_uhdr_cache_var IN LISTS _uhdr_cache_vars)
          if(_uhdr_cache_var MATCHES "^Qt6")
            unset(${_uhdr_cache_var} CACHE)
          endif()
        endforeach()
      endif()
    endif()
  elseif(_uhdr_static_qt)
    list(APPEND CMAKE_PREFIX_PATH "${_uhdr_static_qt}")
  endif()

  find_package(Qt6 6.11 REQUIRED COMPONENTS Core Concurrent Gui GuiPrivate Widgets ShaderTools)

  if(APPLE)
    list(APPEND _UHDR_GUI_SOURCES
      src/gui/web_chrome_mac.mm
      src/gui/app_icon_mac.mm
      src/gui/sdr_preview_image_mac.mm
      src/gui/display_hdr_mac.mm
    )
  elseif(WIN32)
    list(APPEND _UHDR_GUI_SOURCES
      src/gui/web_chrome_win.cpp
      src/gui/sdr_preview_image_win.cpp
    )
  endif()

  list(APPEND _UHDR_ALL_SOURCES ${_UHDR_GUI_SOURCES})
endif()

# Fused multiply-adds change the chart and ICC bytes versus the committed Windows chart.
# MSVC does not contract these expressions.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  set_source_files_properties(src/hdr_chart.cpp src/chart_io.cpp src/hdr_scenes.cpp src/icc_profile.cpp
    PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
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
  target_include_directories(uhdr_repack PRIVATE
    "${libultrahdr_SOURCE_DIR}/third_party/turbojpeg/src"
    "${libultrahdr_BINARY_DIR}/turbojpeg/src/turbojpeg-build"
  )
  # libultrahdr v2 names the static MSVC target uhdr-static. Linking the bare
  # name "uhdr" is then a filename, so Ninja can link before that library exists.
  if(TARGET uhdr-static)
    target_link_libraries(uhdr_repack PRIVATE uhdr-static)
  elseif(TARGET uhdr)
    target_link_libraries(uhdr_repack PRIVATE uhdr)
  else()
    message(FATAL_ERROR "libultrahdr did not create a uhdr or uhdr-static target")
  endif()
  if(WIN32)
    target_link_libraries(uhdr_repack PRIVATE
      "${libultrahdr_BINARY_DIR}/turbojpeg/src/turbojpeg-build/jpeg-static.lib")
  endif()
endif()

target_link_libraries(uhdr_repack PRIVATE nlohmann_json::nlohmann_json)
target_include_directories(uhdr_repack PRIVATE "${miniz_SOURCE_DIR}")
target_compile_definitions(uhdr_repack PRIVATE
  MINIZ_NO_ZLIB_COMPATIBLE_NAMES MINIZ_NO_ARCHIVE_APIS MINIZ_NO_STDIO)

if(UHDR_ENABLE_GUI)
  target_compile_definitions(uhdr_repack PRIVATE UHDR_ENABLE_GUI=1)
  target_link_libraries(uhdr_repack PRIVATE
    Qt6::Core
    Qt6::Concurrent
    Qt6::Gui
    Qt6::GuiPrivate
    Qt6::Widgets
  )
  if(UHDR_STATIC_QT)
    # add_executable does not pull static plugins. The cocoa/windows plugin starts
    # the GUI. PNG is for data-URL previews. Do not import QJpegPlugin: Qt's
    # bundled libjpeg is ABI 80 and libultrahdr's libjpeg-turbo is ABI 62, and
    # one static binary cannot carry both jpeg_* symbols (exit 139).
    set(_uhdr_static_plugins Qt6::QPngPlugin)
    if(APPLE)
      list(APPEND _uhdr_static_plugins Qt6::QCocoaIntegrationPlugin)
    elseif(WIN32)
      list(APPEND _uhdr_static_plugins Qt6::QWindowsIntegrationPlugin)
    endif()
    qt_import_plugins(uhdr_repack INCLUDE ${_uhdr_static_plugins})
  endif()
  if(APPLE)
    find_library(APPKIT_FRAMEWORK AppKit REQUIRED)
    find_library(WEBKIT_FRAMEWORK WebKit REQUIRED)
    find_library(IMAGEIO_FRAMEWORK ImageIO REQUIRED)
    target_link_libraries(uhdr_repack PRIVATE ${APPKIT_FRAMEWORK} ${WEBKIT_FRAMEWORK} ${IMAGEIO_FRAMEWORK})
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

enable_testing()

get_filename_component(UHDR_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(UHDR_TEST_OUT "${CMAKE_CURRENT_BINARY_DIR}/test-out")
set(UHDR_BIN $<TARGET_FILE:uhdr_repack>)
set(UHDR_CHART_DIR "${UHDR_TEST_OUT}/hdr-chart")
set(UHDR_CHART_HDR "${UHDR_CHART_DIR}/hdr-chart.tif")
set(UHDR_CHART_SDR "${UHDR_CHART_DIR}/sdr-chart.jpg")

# Windows CI builds shared Qt, then runs ctest in a new shell. PATH from the
# build step does not carry over, so each test must find Qt6Core.dll itself.
set(_uhdr_qt_test_env "")
if(WIN32 AND NOT UHDR_STATIC_QT AND UHDR_ENABLE_GUI AND TARGET Qt6::Core)
  set(_uhdr_qt_core_dll "")
  if(CMAKE_BUILD_TYPE)
    string(TOUPPER "${CMAKE_BUILD_TYPE}" _uhdr_qt_cfg)
    get_target_property(_uhdr_qt_core_dll Qt6::Core "IMPORTED_LOCATION_${_uhdr_qt_cfg}")
  endif()
  if(NOT _uhdr_qt_core_dll OR _uhdr_qt_core_dll MATCHES "-NOTFOUND$")
    foreach(_uhdr_qt_cfg IN ITEMS RELWITHDEBINFO RELEASE MINSIZEREL DEBUG)
      if(NOT _uhdr_qt_core_dll OR _uhdr_qt_core_dll MATCHES "-NOTFOUND$")
        get_target_property(_uhdr_qt_core_dll Qt6::Core "IMPORTED_LOCATION_${_uhdr_qt_cfg}")
      endif()
    endforeach()
  endif()
  if(_uhdr_qt_core_dll AND NOT _uhdr_qt_core_dll MATCHES "-NOTFOUND$")
    get_filename_component(_uhdr_qt_bin "${_uhdr_qt_core_dll}" DIRECTORY)
    if(_uhdr_qt_bin)
      set(_uhdr_qt_test_env
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_uhdr_qt_bin}")
    endif()
  endif()
endif()

add_test(NAME chart.write COMMAND ${UHDR_BIN} --write-hdr-chart "${UHDR_CHART_DIR}")
set_tests_properties(chart.write PROPERTIES LABELS chart TIMEOUT 600 FIXTURES_SETUP chart ${_uhdr_qt_test_env})

function(uhdr_chart_test name)
  add_test(NAME ${name} COMMAND ${UHDR_BIN} ${ARGN})
  set_tests_properties(${name} PROPERTIES LABELS chart TIMEOUT 600 FIXTURES_REQUIRED chart ${_uhdr_qt_test_env})
endfunction()

uhdr_chart_test(chart.assets-current --check-hdr-chart-assets "${UHDR_REPO_ROOT}/test/hdr-chart" "${UHDR_CHART_DIR}")
uhdr_chart_test(chart.input.hdr --describe-input "${UHDR_CHART_HDR}" --require-portable)
uhdr_chart_test(chart.input.sdr --describe-input "${UHDR_CHART_SDR}" --require-portable)

foreach(preset color mono)
  if(preset STREQUAL "color")
    set(_edge_gate --max-edge-p99 0.30)
  else()
    set(_edge_gate --max-edge-p99 1.34)
  endif()
  uhdr_chart_test(chart.${preset}.original --check-hdr-chart "${UHDR_CHART_DIR}"
    --out "${UHDR_TEST_OUT}/chart-${preset}-original.jpg" --preset ${preset} ${_edge_gate})
  foreach(aspect 1x1 4x5 3x4 191x100)
    uhdr_chart_test(chart.${preset}.ig-${aspect}-1080 --check-hdr-chart "${UHDR_CHART_DIR}"
      --out "${UHDR_TEST_OUT}/chart-${preset}-${aspect}-1080.jpg" --preset ${preset}
      --slice-aspect ${aspect} --out-width 1080)
    uhdr_chart_test(chart.${preset}.ig-${aspect}-native --check-hdr-chart "${UHDR_CHART_DIR}"
      --out "${UHDR_TEST_OUT}/chart-${preset}-${aspect}-native.jpg" --preset ${preset}
      --slice-aspect ${aspect})
  endforeach()
endforeach()

foreach(preset color mono)
  if(preset STREQUAL "color")
    set(_ig_gate --max-edge-p99-instagram 1.95)
  else()
    set(_ig_gate --max-edge-p99-instagram 1.96)
  endif()
  uhdr_chart_test(chart.${preset}.instagram --check-hdr-chart "${UHDR_CHART_DIR}"
    --out "${UHDR_TEST_OUT}/chart-${preset}-ig-src.jpg" --preset ${preset} --simulate-instagram
    ${_ig_gate})
  set_tests_properties(chart.${preset}.instagram PROPERTIES FAIL_REGULAR_EXPRESSION "edge report skipped")
endforeach()

uhdr_chart_test(chart.color.edge-gate-trips --check-hdr-chart "${UHDR_CHART_DIR}"
  --out "${UHDR_TEST_OUT}/chart-color-edge-gate.jpg" --preset color --max-edge-p99 0)
set_tests_properties(chart.color.edge-gate-trips PROPERTIES
  PASS_REGULAR_EXPRESSION "exceeds --max-edge-p99 0.000")

function(uhdr_smoke_pair name verify_args)
  add_test(NAME smoke.${name}.encode COMMAND ${UHDR_BIN} ${ARGN})
  add_test(NAME smoke.${name}.verify COMMAND ${UHDR_BIN} ${verify_args})
  set_tests_properties(smoke.${name}.encode PROPERTIES LABELS smoke TIMEOUT 600
    FIXTURES_REQUIRED chart FIXTURES_SETUP smoke-${name} ${_uhdr_qt_test_env})
  set_tests_properties(smoke.${name}.verify PROPERTIES LABELS smoke TIMEOUT 600
    FIXTURES_REQUIRED smoke-${name} ${_uhdr_qt_test_env})
endfunction()

uhdr_smoke_pair(cli.default
  "--verify-uhdr;${UHDR_TEST_OUT}/cli-default.jpg;--expect-size;1440x1920"
  --hdr-tiff "${UHDR_CHART_HDR}" --base "${UHDR_CHART_SDR}" --out "${UHDR_TEST_OUT}/cli-default.jpg")
uhdr_smoke_pair(cli.slice-1x1
  "--verify-uhdr;${UHDR_TEST_OUT}/cli-1x1.jpg;--expect-size;1440x1440;--expect-single;1x1"
  --hdr-tiff "${UHDR_CHART_HDR}" --base "${UHDR_CHART_SDR}" --out "${UHDR_TEST_OUT}/cli-1x1.jpg"
  --slice-aspect 1x1)
uhdr_smoke_pair(cli.feed-4x5
  "--verify-uhdr;${UHDR_TEST_OUT}/cli-4x5.jpg;--expect-size;1080x1350;--max-bytes;8388608"
  --hdr-tiff "${UHDR_CHART_HDR}" --base "${UHDR_CHART_SDR}" --out "${UHDR_TEST_OUT}/cli-4x5.jpg"
  --slice-aspect 4x5 --out-width 1080)

add_test(NAME smoke.utf8-path COMMAND ${UHDR_BIN} --check-utf8-path
  "${UHDR_CHART_HDR}" "${UHDR_CHART_SDR}" "${UHDR_TEST_OUT}")
set_tests_properties(smoke.utf8-path PROPERTIES LABELS smoke TIMEOUT 600 FIXTURES_REQUIRED chart ${_uhdr_qt_test_env})

add_test(NAME smoke.instagram.encode COMMAND ${UHDR_BIN} --simulate-instagram
  "${UHDR_TEST_OUT}/cli-default.jpg" "${UHDR_TEST_OUT}/cli-default-instagram.jpg")
add_test(NAME smoke.instagram.inspect COMMAND ${UHDR_BIN} --inspect "${UHDR_TEST_OUT}/cli-default-instagram.jpg")
add_test(NAME smoke.instagram.verify COMMAND ${UHDR_BIN} --verify-uhdr "${UHDR_TEST_OUT}/cli-default-instagram.jpg"
  --expect-size 1440x1920)
set_tests_properties(smoke.instagram.encode PROPERTIES LABELS smoke TIMEOUT 600
  FIXTURES_REQUIRED smoke-cli.default FIXTURES_SETUP smoke-instagram ${_uhdr_qt_test_env})
set_tests_properties(smoke.instagram.inspect PROPERTIES LABELS smoke TIMEOUT 600
  FIXTURES_REQUIRED smoke-instagram PASS_REGULAR_EXPRESSION "gainmap_jpeg_420: likely" ${_uhdr_qt_test_env})
set_tests_properties(smoke.instagram.verify PROPERTIES LABELS smoke TIMEOUT 600
  FIXTURES_REQUIRED smoke-instagram ${_uhdr_qt_test_env})

if(UHDR_ENABLE_GUI)
  add_test(NAME smoke.probe-sdr COMMAND ${UHDR_BIN} --probe-sdr "${UHDR_CHART_SDR}")
  add_test(NAME smoke.preview-jpeg COMMAND ${UHDR_BIN} --check-preview-jpeg "${UHDR_CHART_SDR}")
  set_tests_properties(smoke.probe-sdr smoke.preview-jpeg PROPERTIES
    LABELS smoke TIMEOUT 600 FIXTURES_REQUIRED chart FAIL_REGULAR_EXPRESSION "Wrong JPEG library version" ${_uhdr_qt_test_env})
endif()

add_test(NAME scenes.write COMMAND ${UHDR_BIN} --write-hdr-scenes "${UHDR_TEST_OUT}/hdr-scenes")
set_tests_properties(scenes.write PROPERTIES LABELS smoke TIMEOUT 600 FIXTURES_SETUP scenes ${_uhdr_qt_test_env})
foreach(scene sunset neon pastel)
  add_test(NAME scenes.${scene}.encode COMMAND ${UHDR_BIN}
    --hdr-tiff "${UHDR_TEST_OUT}/hdr-scenes/${scene}-hdr.tif" --base "${UHDR_TEST_OUT}/hdr-scenes/${scene}-sdr.jpg"
    --out "${UHDR_TEST_OUT}/hdr-scenes/${scene}-uhdr.jpg")
  add_test(NAME scenes.${scene}.verify COMMAND ${UHDR_BIN} --verify-uhdr "${UHDR_TEST_OUT}/hdr-scenes/${scene}-uhdr.jpg"
    --expect-size 1080x1350)
  set_tests_properties(scenes.${scene}.encode PROPERTIES LABELS smoke TIMEOUT 600
    FIXTURES_REQUIRED scenes FIXTURES_SETUP scene-${scene} ${_uhdr_qt_test_env})
  set_tests_properties(scenes.${scene}.verify PROPERTIES LABELS smoke TIMEOUT 600
    FIXTURES_REQUIRED scene-${scene} ${_uhdr_qt_test_env})
endforeach()

install(TARGETS uhdr_repack RUNTIME DESTINATION bin)
