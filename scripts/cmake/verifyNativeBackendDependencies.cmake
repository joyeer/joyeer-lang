if(NOT LLVM_READOBJ OR NOT JOYEER_EXECUTABLE OR NOT BACKEND_LIBRARY)
    message(FATAL_ERROR
            "LLVM_READOBJ, JOYEER_EXECUTABLE, and BACKEND_LIBRARY are required")
endif()

foreach(binary IN ITEMS "${JOYEER_EXECUTABLE}" "${BACKEND_LIBRARY}")
    execute_process(
            COMMAND "${LLVM_READOBJ}" --coff-imports "${binary}"
            RESULT_VARIABLE readobj_result
            OUTPUT_VARIABLE imports
            ERROR_VARIABLE readobj_error
    )
    if(NOT readobj_result EQUAL 0)
        message(FATAL_ERROR
                "Cannot inspect ${binary}:\n${imports}${readobj_error}")
    endif()
    string(TOLOWER "${imports}" imports_lower)
    if(imports_lower MATCHES
       "name: [^\n]*(llvm|clang|lld|libxml|xml2)[^\n]*\\.dll")
        message(FATAL_ERROR
                "${binary} imports an external compiler dependency:\n${imports}")
    endif()
    if(binary STREQUAL JOYEER_EXECUTABLE AND
       NOT imports_lower MATCHES "name: joyeer-native-backend\\.dll")
        message(FATAL_ERROR
                "${binary} does not import the Joyeer native backend:\n${imports}")
    endif()
endforeach()

if(NOT imports_lower MATCHES
   "name: (kernel32|ntdll|advapi32|bcrypt|oleaut32|shell32|ole32)\\.dll")
    message(FATAL_ERROR "Backend import table contains no expected Windows system DLL")
endif()