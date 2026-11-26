foreach(workers 1 4)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env NEUTRINO_PHYSICS_WORKERS=${workers} "${PROGRAM}" --parallel-fixture
        RESULT_VARIABLE status OUTPUT_VARIABLE simulation ERROR_VARIABLE diagnostics)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "Physics fixture with ${workers} workers failed (${status})\n${diagnostics}")
    endif()
    string(SHA256 digest${workers} "${simulation}")
endforeach()
if(NOT digest1 STREQUAL digest4)
    message(FATAL_ERROR "Physics result changed with worker count")
endif()
