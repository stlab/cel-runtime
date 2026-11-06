# Compiles an invalid opaque registration and requires its specific contract diagnostic.
execute_process(
    COMMAND "${CXX}" -std=c++20 "-I${INCLUDE_DIR}" -c "${SOURCE}" -o "${OUTPUT}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr)
if(result EQUAL 0)
    file(REMOVE "${OUTPUT}")
    message(FATAL_ERROR "Nontrivial opaque leaf registration unexpectedly compiled")
endif()
if(NOT stderr MATCHES "opaque CEL leaves must be trivially copyable")
    message(FATAL_ERROR "Compilation failed for another reason:\n${stdout}\n${stderr}")
endif()
