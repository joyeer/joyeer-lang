if(NOT LLVM_READOBJ OR NOT JOYEER_EXECUTABLE OR NOT BACKEND_LIBRARY OR NOT EXPECTED_MACHINE)
    message(FATAL_ERROR
            "LLVM_READOBJ, JOYEER_EXECUTABLE, BACKEND_LIBRARY, and EXPECTED_MACHINE are required")
endif()

foreach(binary IN ITEMS "${JOYEER_EXECUTABLE}" "${BACKEND_LIBRARY}")
    execute_process(
            COMMAND "${LLVM_READOBJ}" --file-headers --coff-imports "${binary}"
            RESULT_VARIABLE readobj_result
            OUTPUT_VARIABLE binary_info
            ERROR_VARIABLE readobj_error
    )
    if(NOT readobj_result EQUAL 0)
        message(FATAL_ERROR
                "Cannot inspect ${binary}:\n${binary_info}${readobj_error}")
    endif()
    if(NOT binary_info MATCHES "Machine: ${EXPECTED_MACHINE} ")
        message(FATAL_ERROR "${binary} is not ${EXPECTED_MACHINE}:\n${binary_info}")
    endif()
    string(TOLOWER "${binary_info}" binary_info_lower)
    if(binary_info_lower MATCHES
       "name: [^\n]*(llvm|clang|lld|libxml|xml2)[^\n]*\\.dll")
        message(FATAL_ERROR
                "${binary} imports an external compiler dependency:\n${binary_info}")
    endif()
    if(binary STREQUAL JOYEER_EXECUTABLE AND
       NOT binary_info_lower MATCHES "name: joyeer-backend\\.dll")
        message(FATAL_ERROR
                "${binary} does not import the Joyeer native backend:\n${binary_info}")
    endif()
endforeach()

if(NOT binary_info_lower MATCHES
   "name: (kernel32|ntdll|advapi32|bcrypt|oleaut32|shell32|ole32)\\.dll")
    message(FATAL_ERROR "Backend import table contains no expected Windows system DLL")
endif()