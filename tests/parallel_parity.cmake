foreach(workers 1 2 4 8)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env NEUTRINO_PHYSICS_WORKERS=${workers} "${PROGRAM}" --parallel-fixture
        RESULT_VARIABLE status OUTPUT_VARIABLE simulation ERROR_VARIABLE diagnostics)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "Physics fixture with ${workers} workers failed (${status})\n${diagnostics}")
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env NEUTRINO_PHYSICS_WORKERS=${workers} "${PROGRAM}" --scaling-proof
        RESULT_VARIABLE status OUTPUT_VARIABLE scaling ERROR_VARIABLE diagnostics)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "Scaling fixture with ${workers} workers failed (${status})\n${diagnostics}")
    endif()
    string(SHA256 digest${workers} "${simulation}\n${scaling}")
    if(NOT digest1 STREQUAL digest${workers})
        message(FATAL_ERROR "Physics result changed with ${workers} workers")
    endif()
endforeach()
