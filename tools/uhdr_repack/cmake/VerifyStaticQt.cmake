if(NOT DEFINED UHDR_BINARY)
  message(FATAL_ERROR "UHDR_BINARY is required")
endif()

if(APPLE)
  execute_process(
    COMMAND otool -L "${UHDR_BINARY}"
    RESULT_VARIABLE inspect_result
    OUTPUT_VARIABLE dependencies
    ERROR_VARIABLE inspect_error
  )
elseif(WIN32)
  execute_process(
    COMMAND dumpbin /dependents "${UHDR_BINARY}"
    RESULT_VARIABLE inspect_result
    OUTPUT_VARIABLE dependencies
    ERROR_VARIABLE inspect_error
  )
else()
  message(FATAL_ERROR "Static Qt verification supports macOS and Windows only")
endif()

if(NOT inspect_result EQUAL 0)
  message(FATAL_ERROR "Could not inspect ${UHDR_BINARY}: ${inspect_error}")
endif()

if(dependencies MATCHES "Qt[A-Za-z0-9_-]*(\\.framework|\\.dylib|\\.dll)")
  message(FATAL_ERROR
    "UHDR_STATIC_QT=ON but the executable still depends on shared Qt:\n${dependencies}")
endif()

message(STATUS "Verified: ${UHDR_BINARY} has no shared Qt dependencies")
