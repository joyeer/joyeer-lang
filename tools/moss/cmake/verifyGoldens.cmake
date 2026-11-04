# Compare `moss disasm` of each checked-in class with its javap golden, byte for byte.
# Required: MOSS FIXTURES
file(GLOB goldens "${FIXTURES}/*.javap.txt")
if(NOT goldens)
    message(FATAL_ERROR "no goldens under ${FIXTURES}")
endif()
foreach(golden IN LISTS goldens)
    get_filename_component(name "${golden}" NAME)
    string(REPLACE ".javap.txt" ".class" class_name "${name}")
    execute_process(
        COMMAND "${MOSS}" disasm "${FIXTURES}/${class_name}"
        OUTPUT_VARIABLE actual ERROR_VARIABLE err RESULT_VARIABLE status)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "moss disasm ${class_name} failed (${status}): ${err}")
    endif()
    file(READ "${golden}" expected)
    if(NOT actual STREQUAL expected)
        message(FATAL_ERROR "${class_name}: output differs from ${name}")
    endif()
endforeach()
list(LENGTH goldens count)
message(STATUS "moss matched ${count} javap goldens")
