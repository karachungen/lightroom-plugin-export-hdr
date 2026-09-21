# Idempotent: insert jpeg_simple_progression after jpeg_set_quality in libultrahdr v1.4.0.
if(NOT JPEG_ENCODER_CPP)
  message(FATAL_ERROR "JPEG_ENCODER_CPP is required")
endif()
if(NOT EXISTS "${JPEG_ENCODER_CPP}")
  message(FATAL_ERROR "Missing ${JPEG_ENCODER_CPP}")
endif()
file(READ "${JPEG_ENCODER_CPP}" _src)
if(_src MATCHES "jpeg_simple_progression")
  return()
endif()
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
