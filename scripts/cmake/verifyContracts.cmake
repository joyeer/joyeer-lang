cmake_minimum_required(VERSION 3.20)

execute_process(
    COMMAND "${JOYEER_EXECUTABLE}" "${OPTIMIZATION}" -g0 -o "${OUTPUT_FILE}" "${INPUT_FILE}"
    RESULT_VARIABLE compiled OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT compiled STREQUAL "0")
    message(FATAL_ERROR "Contract fixture did not compile (${compiled}):\n${stdout}${stderr}")
endif()

foreach(mode IN ITEMS assert precondition fatal)
    if(mode STREQUAL "fatal")
        # "before" + NUL + FF + "after", not a C string or normalized UTF-8.
        set(message_hex "6265666f726500ff6166746572")
    elseif(mode STREQUAL "assert")
        string(HEX "assert failed" message_hex)
    else()
        string(HEX "precondition failed" message_hex)
    endif()
    execute_process(COMMAND "${OUTPUT_FILE}" "${mode}"
        RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_FILE "${OUTPUT_FILE}.stderr"
        TIMEOUT 20)
    file(READ "${OUTPUT_FILE}.stderr" stderr_hex HEX)
    file(REMOVE "${OUTPUT_FILE}.stderr")
    string(HEX "Joyeer runtime error: " prefix)
    if(NOT status STREQUAL "1" OR NOT stdout STREQUAL "condition\nmessage\n"
        OR NOT stderr_hex STREQUAL "${prefix}${message_hex}0a")
        message(FATAL_ERROR "${mode} produced status ${status}, stdout '${stdout}', stderr ${stderr_hex}")
    endif()
endforeach()
file(REMOVE "${OUTPUT_FILE}")
