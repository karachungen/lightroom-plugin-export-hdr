# Idempotent: insert jpeg_simple_progression after jpeg_set_quality in libultrahdr.
if(NOT JPEG_ENCODER_CPP)
  message(FATAL_ERROR "JPEG_ENCODER_CPP is required")
endif()
if(NOT EXISTS "${JPEG_ENCODER_CPP}")
  message(FATAL_ERROR "Missing ${JPEG_ENCODER_CPP}")
endif()
file(READ "${JPEG_ENCODER_CPP}" _src)
if(NOT _src MATCHES "jpeg_simple_progression")
  set(_needle "    // start compress
    jpeg_start_compress(&cinfo, TRUE);")
  string(FIND "${_src}" "${_needle}" _pos)
  if(_pos LESS 0)
    message(FATAL_ERROR "Could not find jpeg_start_compress in ${JPEG_ENCODER_CPP}")
  endif()
  string(REPLACE "${_needle}"
    "    jpeg_simple_progression(&cinfo);\n    // start compress\n    jpeg_start_compress(&cinfo, TRUE);"
    _out "${_src}")
  file(WRITE "${JPEG_ENCODER_CPP}" "${_out}")
endif()

# libultrahdr's ExternalProject for libjpeg-turbo does not forward the Ninja
# program or the Windows SDK resource tools. A nested cmake then cannot find ninja.
if(NOT LIBUHDR_CMAKE OR NOT EXISTS "${LIBUHDR_CMAKE}")
  return()
endif()
file(READ "${LIBUHDR_CMAKE}" _uhdr)
if(_uhdr MATCHES "CMAKE_MAKE_PROGRAM CMAKE_RC_COMPILER CMAKE_MT")
  return()
endif()
set(_uhdr_needle "set(UHDR_CMAKE_ARGS -DCMAKE_C_COMPILER=\${CMAKE_C_COMPILER})")
string(FIND "${_uhdr}" "${_uhdr_needle}" _uhdr_pos)
if(_uhdr_pos LESS 0)
  message(FATAL_ERROR "Could not find UHDR_CMAKE_ARGS in ${LIBUHDR_CMAKE}")
endif()
string(REPLACE "${_uhdr_needle}"
  "${_uhdr_needle}
foreach(_uhdr_tool CMAKE_MAKE_PROGRAM CMAKE_RC_COMPILER CMAKE_MT)
  if(DEFINED \${_uhdr_tool})
    list(APPEND UHDR_CMAKE_ARGS \"-D\${_uhdr_tool}=\${\${_uhdr_tool}}\")
  endif()
endforeach()"
  _uhdr_out "${_uhdr}")
file(WRITE "${LIBUHDR_CMAKE}" "${_uhdr_out}")
