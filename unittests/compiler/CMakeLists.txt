add_executable(CompilerLexerTests
        lexer_test.cpp
)

add_executable(CompilerDiagnosticTests
        diagnostic_test.cpp
)

target_link_libraries(CompilerDiagnosticTests
        GTest::gtest_main
        JoyeerDiagnostic
)

joyeer_discover_tests(CompilerDiagnosticTests
        PROPERTIES LABELS diagnostics
)

target_link_libraries(CompilerLexerTests
        GTest::gtest_main
        JoyeerDiagnostic
        JoyeerCompiler
)

joyeer_discover_tests(CompilerLexerTests
        PROPERTIES LABELS lexer
)

add_executable(CompilerParserTests
        parser_test.cpp
)

target_link_libraries(CompilerParserTests
        GTest::gtest_main
        JoyeerDiagnostic
        JoyeerCompiler
)

target_compile_definitions(CompilerParserTests PRIVATE
        JOYEER_TESTS_DIR="${JOYEER_TESTS_DIR}"
)

joyeer_discover_tests(CompilerParserTests
        PROPERTIES LABELS parser
)

add_executable(CompilerNameResolutionTests
        nameresolution_test.cpp
)

target_link_libraries(CompilerNameResolutionTests
        GTest::gtest_main
        JoyeerDiagnostic
        JoyeerCompiler
)

target_compile_definitions(CompilerNameResolutionTests PRIVATE
        JOYEER_TESTS_DIR="${JOYEER_TESTS_DIR}"
)

joyeer_discover_tests(CompilerNameResolutionTests
        PROPERTIES LABELS name-resolution
)

add_executable(CompilerTypeCheckingTests
        typechecking_test.cpp
)

target_link_libraries(CompilerTypeCheckingTests
        GTest::gtest_main
        JoyeerDiagnostic
        JoyeerCompiler
)

target_compile_definitions(CompilerTypeCheckingTests PRIVATE
        JOYEER_TESTS_DIR="${JOYEER_TESTS_DIR}"
)

joyeer_discover_tests(CompilerTypeCheckingTests
        PROPERTIES LABELS type-checking
)

add_executable(CompilerSemanticAnalysisTests
        semanticanalysis_test.cpp
)

target_link_libraries(CompilerSemanticAnalysisTests
        GTest::gtest_main
        JoyeerDiagnostic
        JoyeerCompiler
)

joyeer_discover_tests(CompilerSemanticAnalysisTests
        PROPERTIES LABELS semantic-analysis
)

add_executable(IRModelTests
        ir_test.cpp
)

target_link_libraries(IRModelTests
        GTest::gtest_main
        JoyeerIR
)

joyeer_discover_tests(IRModelTests
        PROPERTIES LABELS ir
)

add_executable(IRLoweringTests
        irlowering_test.cpp
)

target_link_libraries(IRLoweringTests
        GTest::gtest_main
        JoyeerDiagnostic
        JoyeerCompiler
        JoyeerIR
)

target_compile_definitions(IRLoweringTests PRIVATE
        JOYEER_TESTS_DIR="${JOYEER_TESTS_DIR}"
)

joyeer_discover_tests(IRLoweringTests
        PROPERTIES LABELS ir-lowering
)

add_executable(LLVMBackendTests
        llvm_test.cpp
)

target_link_libraries(LLVMBackendTests
        GTest::gtest_main
        JoyeerDiagnostic
        JoyeerCompiler
        JoyeerLLVMBackend
)

joyeer_discover_tests(LLVMBackendTests
        PROPERTIES LABELS llvm-backend
)

if(WIN32)
    add_executable(NativeBackendAbiTests
            native_backend_abi_test.c
    )
    target_link_libraries(NativeBackendAbiTests
            JoyeerNativeBackend
    )
    add_test(NAME NativeBackendAbiTests COMMAND NativeBackendAbiTests)
    set_tests_properties(NativeBackendAbiTests PROPERTIES
            LABELS "llvm-backend;native;abi"
    )
endif()

if(JOYEER_CLANG_EXECUTABLE)
        get_filename_component(JOYEER_LLVM_BIN_DIR "${JOYEER_CLANG_EXECUTABLE}" DIRECTORY)
        find_program(JOYEER_LLVM_READOBJ_EXECUTABLE
                        NAMES
                                        llvm-readobj llvm-readobj.exe
                                        llvm-readobj-22 llvm-readobj-21 llvm-readobj-20
                                        llvm-readobj-19 llvm-readobj-18 llvm-readobj-17
                        HINTS "${JOYEER_LLVM_BIN_DIR}"
        )
        if(APPLE)
                set(JOYEER_DWARF_LINE_SECTION __debug_line)
        else()
                set(JOYEER_DWARF_LINE_SECTION .debug_line)
        endif()

    add_executable(LLVMFixtureGenerator
            llvm_fixture.cpp
    )
    find_program(JOYEER_LLVM_PDBUTIL_EXECUTABLE
            NAMES llvm-pdbutil llvm-pdbutil.exe
            HINTS "${JOYEER_LLVM_BIN_DIR}"
    )
    find_program(JOYEER_DSYMUTIL_EXECUTABLE
            NAMES dsymutil
            HINTS "${JOYEER_LLVM_BIN_DIR}" /usr/bin
    )
    target_link_libraries(LLVMFixtureGenerator
            JoyeerLLVMBackend
    )

    add_test(NAME LLVMBackendClangValidation
            COMMAND ${CMAKE_COMMAND}
                    -DGENERATOR=$<TARGET_FILE:LLVMFixtureGenerator>
                    -DCLANG=${JOYEER_CLANG_EXECUTABLE}
                    -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/joyeer-llvm-validation.ll
                    -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyLLVMIR.cmake
    )
    set_tests_properties(LLVMBackendClangValidation PROPERTIES
            LABELS llvm-backend
    )

    add_test(NAME LLVMBackendDebugInfoClangValidation
            COMMAND ${CMAKE_COMMAND}
                    -DGENERATOR=$<TARGET_FILE:LLVMFixtureGenerator>
                    -DGENERATOR_ARGS=--debug-dwarf
                    -DCLANG=${JOYEER_CLANG_EXECUTABLE}
                    -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/joyeer-llvm-debug-validation.ll
                    -DEXPECTED_IR_PATTERN=!llvm.dbg.cu
                    -DLLVM_READOBJ=${JOYEER_LLVM_READOBJ_EXECUTABLE}
                    -DEXPECTED_OBJECT_PATTERNS=${JOYEER_DWARF_LINE_SECTION}
                    -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyLLVMIR.cmake
    )
    set_tests_properties(LLVMBackendDebugInfoClangValidation PROPERTIES
            LABELS "llvm-backend;debug-info"
    )

    if(WIN32)
        add_test(NAME LLVMBackendCodeViewClangValidation
                COMMAND ${CMAKE_COMMAND}
                        -DGENERATOR=$<TARGET_FILE:LLVMFixtureGenerator>
                        -DGENERATOR_ARGS=--debug-codeview
                        -DCLANG=${JOYEER_CLANG_EXECUTABLE}
                        -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/joyeer-llvm-codeview-validation.ll
                        "-DEXPECTED_IR_PATTERN=!\"CodeView\""
                        -DLLVM_READOBJ=${JOYEER_LLVM_READOBJ_EXECUTABLE}
                        -DREADOBJ_CODEVIEW=ON
                        "-DEXPECTED_OBJECT_PATTERNS=.debug$S;SubSectionType: Lines;clang-validation.joyeer;LinkageName: joyeer_fn_0;LineNumberStart: 1"
                        -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyLLVMIR.cmake
        )
        set_tests_properties(LLVMBackendCodeViewClangValidation PROPERTIES
                LABELS "llvm-backend;debug-info"
        )
    endif()

    add_test(NAME LLVMBackendCompilerJsonMvpValidation
            COMMAND ${CMAKE_COMMAND}
                    -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                    -DCLANG=${JOYEER_CLANG_EXECUTABLE}
                    -DINPUT_FILE=${JOYEER_TESTS_DIR}/parser/ok/json_mvp.joyeer
                    -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/joyeer-json-mvp.ll
                    "-DUNEXPECTED_IR_PATTERNS=!llvm.dbg.cu;!DILocation"
                    -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyCompilerLLVMOutput.cmake
    )
    set_tests_properties(LLVMBackendCompilerJsonMvpValidation PROPERTIES
            LABELS "llvm-backend;ir-lowering;type-checking"
    )

    add_test(NAME LLVMBackendCompilerDwarfO0Validation
            COMMAND ${CMAKE_COMMAND}
                    -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                    -DCLANG=${JOYEER_CLANG_EXECUTABLE}
                    -DINPUT_FILE=${JOYEER_TESTS_DIR}/native/hello.joyeer
                    -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/joyeer-debug-dwarf-o0.ll
                    "-DCOMPILER_ARGS=-O0;-gdwarf"
                    "-DEXPECTED_IR_PATTERNS=!llvm.dbg.cu;Dwarf Version;isOptimized: false;!DILocation"
                    "-DUNEXPECTED_IR_PATTERNS=CodeView;DISPFlagOptimized"
                    -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyCompilerLLVMOutput.cmake
    )
    set_tests_properties(LLVMBackendCompilerDwarfO0Validation PROPERTIES
            LABELS "llvm-backend;debug-info;optimization"
    )

    add_test(NAME LLVMBackendCompilerDwarfO2Validation
            COMMAND ${CMAKE_COMMAND}
                    -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                    -DCLANG=${JOYEER_CLANG_EXECUTABLE}
                    -DINPUT_FILE=${JOYEER_TESTS_DIR}/native/hello.joyeer
                    -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/joyeer-debug-dwarf-o2.ll
                    "-DCOMPILER_ARGS=-O2;-gdwarf"
                    "-DEXPECTED_IR_PATTERNS=!llvm.dbg.cu;Dwarf Version;isOptimized: true;DISPFlagOptimized"
                    -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyCompilerLLVMOutput.cmake
    )
    set_tests_properties(LLVMBackendCompilerDwarfO2Validation PROPERTIES
            LABELS "llvm-backend;debug-info;optimization"
    )

    add_test(NAME LLVMBackendCompilerFullDebugValidation
            COMMAND ${CMAKE_COMMAND}
                    -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                    -DCLANG=${JOYEER_CLANG_EXECUTABLE}
                    -DINPUT_FILE=${JOYEER_TESTS_DIR}/native/full_debug.joyeer
                    -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/joyeer-full-debug.ll
                    "-DCOMPILER_ARGS=-O0;-gfull;-gdwarf"
                    "-DEXPECTED_IR_PATTERNS=emissionKind: FullDebug;llvm.dbg.declare;DILocalVariable;DILexicalBlock;DIBasicType;DICompositeType"
                    "-DUNEXPECTED_IR_PATTERNS=emissionKind: LineTablesOnly;#dbg_declare"
                    -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyCompilerLLVMOutput.cmake
    )
    set_tests_properties(LLVMBackendCompilerFullDebugValidation PROPERTIES
            LABELS "llvm-backend;debug-info"
    )

    if(WIN32)
        add_test(NAME LLVMBackendCompilerPlatformDebugValidation
                COMMAND ${CMAKE_COMMAND}
                        -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                        -DCLANG=${JOYEER_CLANG_EXECUTABLE}
                        -DINPUT_FILE=${JOYEER_TESTS_DIR}/native/hello.joyeer
                        -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/joyeer-debug-platform.ll
                        -DCOMPILER_ARGS=-g
                        "-DEXPECTED_IR_PATTERNS=!llvm.dbg.cu;CodeView;!DILocation"
                        "-DUNEXPECTED_IR_PATTERNS=Dwarf Version"
                        -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyCompilerLLVMOutput.cmake
        )
        set_tests_properties(LLVMBackendCompilerPlatformDebugValidation PROPERTIES
                LABELS "llvm-backend;debug-info"
        )
    endif()

    add_test(NAME NativeExecutableHello
            COMMAND ${CMAKE_COMMAND}
                    -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                    -DINPUT_FILE=${JOYEER_TESTS_DIR}/native/hello.joyeer
                    -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/joyeer-native-hello${CMAKE_EXECUTABLE_SUFFIX}
                    -DEXPECTED_OUTPUT=42\nnative\n8\n42\n
                    -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake
    )
    set_tests_properties(NativeExecutableHello PROPERTIES
            LABELS "native;llvm-backend;native-runtime"
    )

    add_test(NAME NativeExecutableNoDebugArtifact
            COMMAND ${CMAKE_COMMAND}
                    -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                    -DINPUT_FILE=${JOYEER_TESTS_DIR}/native/hello.joyeer
                    -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/joyeer-native-no-debug${CMAKE_EXECUTABLE_SUFFIX}
                    "-DCOMPILER_ARGS=-O0;-g0"
                    -DEXPECT_NO_DEBUG_ARTIFACT=ON
                    -DEXPECTED_OUTPUT=42\nnative\n8\n42\n
                    -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake
    )
    set_tests_properties(NativeExecutableNoDebugArtifact PROPERTIES
            LABELS "native;llvm-backend;native-runtime;debug-info;optimization"
    )

    add_test(NAME NativeExecutableMetacharacterPath
            COMMAND ${CMAKE_COMMAND}
                    -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                    -DINPUT_FILE=${JOYEER_TESTS_DIR}/native/hello.joyeer
                    "-DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/joyeer native $() %PATH% & safe${CMAKE_EXECUTABLE_SUFFIX}"
                    -DEXPECT_NO_DEBUG_ARTIFACT=ON
                    -DEXPECTED_OUTPUT=42\nnative\n8\n42\n
                    -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake
    )
    set_tests_properties(NativeExecutableMetacharacterPath PROPERTIES
            LABELS "native;llvm-backend;native-runtime;security"
    )

    add_test(NAME NativeLinkerPreservesOutputDirectory
            COMMAND ${CMAKE_COMMAND}
                    -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                    -DINPUT_FILE=${JOYEER_TESTS_DIR}/native/hello.joyeer
                    -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/joyeer-protected-output
                    -DARTIFACT_KIND=OUTPUT
                    -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeArtifactDirectoryRejected.cmake
    )
    set_tests_properties(NativeLinkerPreservesOutputDirectory PROPERTIES
            LABELS "native;llvm-backend;security"
    )

    add_test(NAME NativeLinkerRejectsSourceOutputCollision
            COMMAND ${CMAKE_COMMAND}
                    -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                    -DSOURCE_TEMPLATE=${JOYEER_TESTS_DIR}/native/hello.joyeer
                    -DTEST_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/source-output-collision
                    -DCOLLISION_KIND=OUTPUT
                    -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifySourceOutputCollisionRejected.cmake
    )
    set_tests_properties(NativeLinkerRejectsSourceOutputCollision PROPERTIES
            LABELS "native;llvm-backend;security"
    )

    add_test(NAME LLVMEmitterRejectsSourceOutputCollision
            COMMAND ${CMAKE_COMMAND}
                    -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                    -DSOURCE_TEMPLATE=${JOYEER_TESTS_DIR}/native/hello.joyeer
                    -DTEST_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/source-llvm-collision
                    -DCOLLISION_KIND=LLVM
                    -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifySourceOutputCollisionRejected.cmake
    )
    set_tests_properties(LLVMEmitterRejectsSourceOutputCollision PROPERTIES
            LABELS "llvm-backend;security"
    )

        if(WIN32 AND JOYEER_LLVM_READOBJ_EXECUTABLE AND JOYEER_LLVM_PDBUTIL_EXECUTABLE)
        add_test(NAME NativeExecutableCodeViewPdb
                COMMAND ${CMAKE_COMMAND}
                        -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                        -DINPUT_FILE=${JOYEER_TESTS_DIR}/native/hello.joyeer
                        -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/joyeer-native-codeview${CMAKE_EXECUTABLE_SUFFIX}
                        "-DCOMPILER_ARGS=-O0;-gcodeview"
                        -DEXPECT_PDB=ON
                        -DLLVM_READOBJ=${JOYEER_LLVM_READOBJ_EXECUTABLE}
                        -DREADOBJ_COFF_DEBUG_DIRECTORY=ON
                        "-DEXPECTED_DEBUG_PATTERNS=Type: CodeView;PDBFileName:"
                        -DLLVM_PDBUTIL=${JOYEER_LLVM_PDBUTIL_EXECUTABLE}
                        "-DEXPECTED_PDB_PATTERNS=hello.joyeer;Lines"
                        -DEXPECTED_OUTPUT=42\nnative\n8\n42\n
                        -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake
        )
        set_tests_properties(NativeExecutableCodeViewPdb PROPERTIES
                LABELS "native;llvm-backend;native-runtime;debug-info"
        )

        add_test(NAME NativeExecutableFullDebugPdb
                COMMAND ${CMAKE_COMMAND}
                        -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                        -DINPUT_FILE=${JOYEER_TESTS_DIR}/native/full_debug.joyeer
                        -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/joyeer-native-full-debug${CMAKE_EXECUTABLE_SUFFIX}
                        "-DCOMPILER_ARGS=-O0;-gfull;-gcodeview"
                        -DEXPECT_PDB=ON
                        -DLLVM_READOBJ=${JOYEER_LLVM_READOBJ_EXECUTABLE}
                        -DREADOBJ_COFF_DEBUG_DIRECTORY=ON
                        "-DEXPECTED_DEBUG_PATTERNS=Type: CodeView;PDBFileName:"
                        -DLLVM_PDBUTIL=${JOYEER_LLVM_PDBUTIL_EXECUTABLE}
                        "-DEXPECTED_PDB_PATTERNS=full_debug.joyeer;S_LOCAL;`plain`;`target`;`payload`;LF_STRUCTURE;`Pair`"
                        -DEXPECTED_OUTPUT=3\n7\n5\n5\n
                        -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake
        )
        set_tests_properties(NativeExecutableFullDebugPdb PROPERTIES
                LABELS "native;llvm-backend;native-runtime;debug-info"
        )

        add_test(NAME NativeLinkerPreservesPdbDirectory
                COMMAND ${CMAKE_COMMAND}
                        -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                        -DINPUT_FILE=${JOYEER_TESTS_DIR}/native/hello.joyeer
                        -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/joyeer-protected-pdb.exe
                        -DARTIFACT_KIND=PDB
                        -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeArtifactDirectoryRejected.cmake
        )
        set_tests_properties(NativeLinkerPreservesPdbDirectory PROPERTIES
                LABELS "native;llvm-backend;debug-info;security"
        )

        add_test(NAME NativeLinkerRejectsSourcePdbCollision
                COMMAND ${CMAKE_COMMAND}
                        -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                        -DSOURCE_TEMPLATE=${JOYEER_TESTS_DIR}/native/hello.joyeer
                        -DTEST_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/source-pdb-collision
                        -DCOLLISION_KIND=PDB
                        -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifySourceOutputCollisionRejected.cmake
        )
        set_tests_properties(NativeLinkerRejectsSourcePdbCollision PROPERTIES
                LABELS "native;llvm-backend;debug-info;security"
        )

        add_test(NAME NativeLinkerRejectsNoDebugSourcePdbCollision
                COMMAND ${CMAKE_COMMAND}
                        -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                        -DSOURCE_TEMPLATE=${JOYEER_TESTS_DIR}/native/hello.joyeer
                        -DTEST_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/source-nodebug-pdb-collision
                        -DCOLLISION_KIND=PDB_NODEBUG
                        -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifySourceOutputCollisionRejected.cmake
        )
        set_tests_properties(NativeLinkerRejectsNoDebugSourcePdbCollision PROPERTIES
                LABELS "native;llvm-backend;security"
        )

        add_test(NAME NativeLinkerRemovesStalePdb
                COMMAND ${CMAKE_COMMAND}
                        -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                        -DINPUT_FILE=${JOYEER_TESTS_DIR}/native/hello.joyeer
                        -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/joyeer-debug-transition.exe
                        -DARTIFACT_KIND=PDB
                        -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeDebugArtifactTransition.cmake
        )
        set_tests_properties(NativeLinkerRemovesStalePdb PROPERTIES
                LABELS "native;llvm-backend;debug-info"
        )

        add_test(NAME NativeLinkerRejectsPdbOutputCollision
                COMMAND ${CMAKE_COMMAND}
                        -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                        -DINPUT_FILE=${JOYEER_TESTS_DIR}/native/hello.joyeer
                        -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/joyeer-output-collision.PDB
                        -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativePdbCollisionRejected.cmake
        )
        set_tests_properties(NativeLinkerRejectsPdbOutputCollision PROPERTIES
                LABELS "native;llvm-backend;debug-info;security"
        )

        add_test(NAME NativeLinkerRejectsTrailingSpacePdbAlias
                COMMAND ${CMAKE_COMMAND}
                        -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                        -DINPUT_FILE=${JOYEER_TESTS_DIR}/native/hello.joyeer
                        "-DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/joyeer-output-alias.pdb "
                        -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativePdbCollisionRejected.cmake
        )
        set_tests_properties(NativeLinkerRejectsTrailingSpacePdbAlias PROPERTIES
                LABELS "native;llvm-backend;debug-info;security"
        )

        add_test(NAME NativeLinkerRejectsAlternateDataStreamOutput
                COMMAND ${CMAKE_COMMAND}
                        -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                        -DINPUT_FILE=${JOYEER_TESTS_DIR}/native/hello.joyeer
                        "-DEXTRA_ARGS=-o;${CMAKE_CURRENT_BINARY_DIR}/joyeer-output.exe:stream"
                        "-DEXPECTED_PATTERN=alternate data streams"
                        -P ${JOYEER_SOURCE_DIR}/scripts/cmake/expectCompilerFailure.cmake
        )
        set_tests_properties(NativeLinkerRejectsAlternateDataStreamOutput PROPERTIES
                LABELS "native;llvm-backend;security"
        )
        endif()

        if(WIN32 AND JOYEER_LLVM_READOBJ_EXECUTABLE)
        add_test(NAME NativeExecutableDwarfArtifact
                COMMAND ${CMAKE_COMMAND}
                        -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                        -DINPUT_FILE=${JOYEER_TESTS_DIR}/native/hello.joyeer
                        -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/joyeer-native-dwarf${CMAKE_EXECUTABLE_SUFFIX}
                        "-DCOMPILER_ARGS=-O2;-gdwarf"
                        -DLLVM_READOBJ=${JOYEER_LLVM_READOBJ_EXECUTABLE}
                        "-DEXPECTED_DEBUG_PATTERNS=.debug_info;.debug_line"
                        -DEXPECTED_OUTPUT=42\nnative\n8\n42\n
                        -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake
        )
        set_tests_properties(NativeExecutableDwarfArtifact PROPERTIES
                LABELS "native;llvm-backend;native-runtime;debug-info;optimization"
        )
        elseif(APPLE AND JOYEER_LLVM_READOBJ_EXECUTABLE AND JOYEER_DSYMUTIL_EXECUTABLE)
        add_test(NAME NativeExecutableDwarfArtifact
                COMMAND ${CMAKE_COMMAND}
                        -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                        -DINPUT_FILE=${JOYEER_TESTS_DIR}/native/hello.joyeer
                        -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/joyeer-native-dwarf${CMAKE_EXECUTABLE_SUFFIX}
                        "-DCOMPILER_ARGS=-O2;-gdwarf"
                        -DEXPECT_DSYM=ON
                        -DLLVM_READOBJ=${JOYEER_LLVM_READOBJ_EXECUTABLE}
                        -DEXPECTED_DEBUG_PATTERNS=__debug_line
                        -DEXPECTED_OUTPUT=42\nnative\n8\n42\n
                        -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake
        )
        set_tests_properties(NativeExecutableDwarfArtifact PROPERTIES
                LABELS "native;llvm-backend;native-runtime;debug-info;optimization"
        )

        add_test(NAME NativeLinkerRemovesStaleDsym
                COMMAND ${CMAKE_COMMAND}
                        -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                        -DINPUT_FILE=${JOYEER_TESTS_DIR}/native/hello.joyeer
                        -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/joyeer-debug-transition
                        -DARTIFACT_KIND=DSYM
                        -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeDebugArtifactTransition.cmake
        )
        set_tests_properties(NativeLinkerRemovesStaleDsym PROPERTIES
                LABELS "native;llvm-backend;debug-info"
        )
        elseif(UNIX AND NOT APPLE AND JOYEER_LLVM_READOBJ_EXECUTABLE)
        add_test(NAME NativeExecutableDwarfArtifact
                COMMAND ${CMAKE_COMMAND}
                        -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                        -DINPUT_FILE=${JOYEER_TESTS_DIR}/native/hello.joyeer
                        -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/joyeer-native-dwarf${CMAKE_EXECUTABLE_SUFFIX}
                        "-DCOMPILER_ARGS=-O2;-gdwarf"
                        -DLLVM_READOBJ=${JOYEER_LLVM_READOBJ_EXECUTABLE}
                        -DEXPECTED_DEBUG_PATTERNS=.debug_line
                        -DEXPECTED_OUTPUT=42\nnative\n8\n42\n
                        -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake
        )
        set_tests_properties(NativeExecutableDwarfArtifact PROPERTIES
                LABELS "native;llvm-backend;native-runtime;debug-info;optimization"
        )
    endif()

    add_test(NAME NativeExecutableOwnership
            COMMAND ${CMAKE_COMMAND}
                    -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                    -DINPUT_FILE=${JOYEER_TESTS_DIR}/native/ownership.joyeer
                    -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/joyeer-native-ownership${CMAKE_EXECUTABLE_SUFFIX}
                    -DEXPECTED_OUTPUT=left!\nleft!\nbeta\nvalue\nsame\n
                    -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake
    )
    set_tests_properties(NativeExecutableOwnership PROPERTIES
            LABELS "native;llvm-backend;native-runtime;ownership"
    )

    add_test(NAME NativeExecutableTrailingReturn
            COMMAND ${CMAKE_COMMAND}
                    -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                    -DINPUT_FILE=${JOYEER_TESTS_DIR}/native/trailing_return.joyeer
                    -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/joyeer-native-trailing-return${CMAKE_EXECUTABLE_SUFFIX}
                    -DEXPECTED_OUTPUT=42\ntrailing\n
                    -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake
    )
    set_tests_properties(NativeExecutableTrailingReturn PROPERTIES
            LABELS "native;llvm-backend;native-runtime;semantic-analysis"
    )

    add_test(NAME NativeExecutableDeferredInitialization
            COMMAND ${CMAKE_COMMAND}
                    -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                    -DINPUT_FILE=${JOYEER_TESTS_DIR}/native/deferred_initialization.joyeer
                    -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/joyeer-native-deferred-initialization${CMAKE_EXECUTABLE_SUFFIX}
                    -DEXPECTED_OUTPUT=42\ninitialized\n
                    -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake
    )
    set_tests_properties(NativeExecutableDeferredInitialization PROPERTIES
            LABELS "native;llvm-backend;native-runtime;semantic-analysis;ownership"
    )

    add_test(NAME NativeExecutableReadFile
            COMMAND ${CMAKE_COMMAND}
                    -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                    -DINPUT_FILE=${JOYEER_TESTS_DIR}/native/read_file.joyeer
                    -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/joyeer-native-read-file${CMAKE_EXECUTABLE_SUFFIX}
                    "-DEXPECTED_OUTPUT=file contents\nnot found\ntrue\n"
                    -DPROGRAM_WORKING_DIRECTORY=${JOYEER_TESTS_DIR}/native
                    -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake
    )
    set_tests_properties(NativeExecutableReadFile PROPERTIES
            LABELS "native;llvm-backend;native-runtime;ownership;file-io"
    )

    add_test(NAME NativeExecutableArrayAppend
            COMMAND ${CMAKE_COMMAND}
                    -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                    -DINPUT_FILE=${JOYEER_TESTS_DIR}/native/array_append.joyeer
                    -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/joyeer-native-array-append${CMAKE_EXECUTABLE_SUFFIX}
                    -DEXPECTED_OUTPUT=3\n30\nfirst!\nsecond\nsecond\n2\n6\n
                    -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake
    )
    set_tests_properties(NativeExecutableArrayAppend PROPERTIES
            LABELS "native;llvm-backend;native-runtime;ownership;array"
    )

    add_test(NAME NativeExecutableDictionarySet
            COMMAND ${CMAKE_COMMAND}
                    -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                    -DINPUT_FILE=${JOYEER_TESTS_DIR}/native/dictionary_set.joyeer
                    -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/joyeer-native-dictionary-set${CMAKE_EXECUTABLE_SUFFIX}
                    -DEXPECTED_OUTPUT=3\nupdated!\ntwo\nretained\nretained\n2\n6\n
                    -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake
    )
    set_tests_properties(NativeExecutableDictionarySet PROPERTIES
            LABELS "native;llvm-backend;native-runtime;ownership;dictionary"
    )

    add_test(NAME NativeExecutableByteConversion
            COMMAND ${CMAKE_COMMAND}
                    -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                    -DINPUT_FILE=${JOYEER_TESTS_DIR}/native/byte_conversion.joyeer
                    -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/joyeer-native-byte-conversion${CMAKE_EXECUTABLE_SUFFIX}
                    -DEXPECTED_OUTPUT=7\nJ\nJoy\n
                    -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake
    )
    set_tests_properties(NativeExecutableByteConversion PROPERTIES
            LABELS "native;llvm-backend;native-runtime;ownership;byte-conversion"
    )

    add_test(NAME NativeExecutableStringUtf8
            COMMAND ${CMAKE_COMMAND}
                    -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                    -DINPUT_FILE=${JOYEER_TESTS_DIR}/native/string_utf8.joyeer
                    -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/joyeer-native-string-utf8${CMAKE_EXECUTABLE_SUFFIX}
                    -DEXPECTED_OUTPUT=Joy\n3\n66\n111\n121\n0\n3\n104\n195\n169\n
                    -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake
    )
    set_tests_properties(NativeExecutableStringUtf8 PROPERTIES
            LABELS "native;llvm-backend;native-runtime;ownership;string;utf8"
    )

    add_test(NAME NativeExecutableJsonParser
            COMMAND ${CMAKE_COMMAND}
                    -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                    -DINPUT_FILE=${JOYEER_TESTS_DIR}/native/json_parser.joyeer
                    -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/joyeer-native-json-parser${CMAKE_EXECUTABLE_SUFFIX}
                    "-DEXPECTED_OUTPUT=true\njoyeer\ntrue\n4\n10\nok\ntrue\n-7\nline\nquote: \"ok\"\ntrue\n"
                    -DPROGRAM_WORKING_DIRECTORY=${JOYEER_TESTS_DIR}/native
                    -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake
    )
    set_tests_properties(NativeExecutableJsonParser PROPERTIES
            LABELS "native;llvm-backend;native-runtime;ownership;file-io;array;dictionary;byte-conversion;json-parser"
    )

    add_test(NAME NativeExecutableConsuming
            COMMAND ${CMAKE_COMMAND}
                    -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                    -DINPUT_FILE=${JOYEER_TESTS_DIR}/native/consuming.joyeer
                    -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/joyeer-native-consuming${CMAKE_EXECUTABLE_SUFFIX}
                    -DEXPECTED_OUTPUT=first!\nsecond\nsecond\ntemporary!\nforwarded!\n2\nreinitialized\n
                    -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake
    )
    set_tests_properties(NativeExecutableConsuming PROPERTIES
            LABELS "native;llvm-backend;native-runtime;ownership;consuming"
    )

    add_test(NAME NativeExecutableInitializing
            COMMAND ${CMAKE_COMMAND}
                    -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                    -DINPUT_FILE=${JOYEER_TESTS_DIR}/native/initializing.joyeer
                    -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/joyeer-native-initializing${CMAKE_EXECUTABLE_SUFFIX}
                    -DEXPECTED_OUTPUT=created!\ncreated!\nforwarded!\n2\nfirst\nsecond\n
                    -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake
    )
    set_tests_properties(NativeExecutableInitializing PROPERTIES
            LABELS "native;llvm-backend;native-runtime;ownership;initializing"
    )

    add_test(NAME NativeExecutableProjectionConsume
            COMMAND ${CMAKE_COMMAND}
                    -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                    -DINPUT_FILE=${JOYEER_TESTS_DIR}/native/projection_consume.joyeer
                    -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/joyeer-native-projection-consume${CMAKE_EXECUTABLE_SUFFIX}
                    -DEXPECTED_OUTPUT=left\nright\nnew-left\nfirst\nnew-first\nsecond\nold\nnew\n
                    -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake
    )
    set_tests_properties(NativeExecutableProjectionConsume PROPERTIES
            LABELS "native;llvm-backend;native-runtime;ownership;consuming;projection-consume"
    )

    add_test(NAME NativeOptimizedOverflowTrap
            COMMAND ${CMAKE_COMMAND}
                    -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                    -DINPUT_FILE=${JOYEER_TESTS_DIR}/native/optimized_overflow.joyeer
                    -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/joyeer-native-optimized-overflow${CMAKE_EXECUTABLE_SUFFIX}
                    "-DEXPECTED_PATTERN=integer addition overflow"
                    -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyOptimizedNativeFailure.cmake
    )
    set_tests_properties(NativeOptimizedOverflowTrap PROPERTIES
            LABELS "native;llvm-backend;native-runtime;optimization;safety"
    )

    add_test(NAME NativeOptimizedBoundsTrap
            COMMAND ${CMAKE_COMMAND}
                    -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                    -DINPUT_FILE=${JOYEER_TESTS_DIR}/native/optimized_bounds.joyeer
                    -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/joyeer-native-optimized-bounds${CMAKE_EXECUTABLE_SUFFIX}
                    "-DEXPECTED_PATTERN=array index out of bounds"
                    -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyOptimizedNativeFailure.cmake
    )
    set_tests_properties(NativeOptimizedBoundsTrap PROPERTIES
            LABELS "native;llvm-backend;native-runtime;optimization;safety"
    )
endif()

add_test(NAME NameResolutionCliRejectsUndefinedName
        COMMAND ${CMAKE_COMMAND}
                -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                -DINPUT_FILE=${JOYEER_TESTS_DIR}/name-resolution/err/undefined_name.joyeer
                -DEXPECTED_PATTERN=name-resolution.undefined-name
                -P ${JOYEER_SOURCE_DIR}/scripts/cmake/expectCompilerFailure.cmake
)
set_tests_properties(NameResolutionCliRejectsUndefinedName PROPERTIES
        LABELS name-resolution
)

add_test(NAME TypeCheckingCliRejectsMismatch
        COMMAND ${CMAKE_COMMAND}
                -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                -DINPUT_FILE=${JOYEER_TESTS_DIR}/type-checking/err/type_mismatch.joyeer
                -DEXPECTED_PATTERN=type-checking.type-mismatch
                -P ${JOYEER_SOURCE_DIR}/scripts/cmake/expectCompilerFailure.cmake
)
set_tests_properties(TypeCheckingCliRejectsMismatch PROPERTIES
        LABELS type-checking
)

add_test(NAME TypeCheckingCliRejectsOverlappingAccess
        COMMAND ${CMAKE_COMMAND}
                -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                -DINPUT_FILE=${JOYEER_TESTS_DIR}/type-checking/err/overlapping_access.joyeer
                "-DEXPECTED_HEADER=overlapping_access.joyeer:7:30: error[type-checking.overlapping-access]"
                "-DEXPECTED_SOURCE=    update(dst: &value, src: value)"
                "-DEXPECTED_NOTE_HEADER=overlapping_access.joyeer:7:18: note: conflicting inout access occurs here"
                -P ${JOYEER_SOURCE_DIR}/scripts/cmake/expectRichCompilerDiagnostic.cmake
)
set_tests_properties(TypeCheckingCliRejectsOverlappingAccess PROPERTIES
        LABELS "diagnostics;type-checking;exclusivity"
)

add_test(NAME DiagnosticsCliSuggestsConsumeMarkerFix
        COMMAND ${CMAKE_COMMAND}
                -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                -DINPUT_FILE=${JOYEER_TESTS_DIR}/type-checking/err/missing_consume_marker.joyeer
                "-DEXPECTED_HEADER=missing_consume_marker.joyeer:7:10: error[type-checking.invalid-consume-argument]"
                "-DEXPECTED_SOURCE=    take(value: text)"
                "-DEXPECTED_HELP=help: insert 'consume' before this argument"
                "-DEXPECTED_FIX_IT=fix-it: missing_consume_marker.joyeer:7:17:0: \"consume \""
                -P ${JOYEER_SOURCE_DIR}/scripts/cmake/expectRichCompilerDiagnostic.cmake
)
set_tests_properties(DiagnosticsCliSuggestsConsumeMarkerFix PROPERTIES
        LABELS "diagnostics;type-checking;fix-it;consuming"
)

add_test(NAME DiagnosticsCliRendersSourceContext
        COMMAND ${CMAKE_COMMAND}
                -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                -DINPUT_FILE=${JOYEER_TESTS_DIR}/type-checking/err/type_mismatch.joyeer
                "-DEXPECTED_HEADER=type_mismatch.joyeer:2:12: error[type-checking.type-mismatch]"
                "-DEXPECTED_SOURCE=    return \"not an integer\""
                "-DEXPECTED_HELP=help: expected 'Int' but found 'String'"
                -P ${JOYEER_SOURCE_DIR}/scripts/cmake/expectRichCompilerDiagnostic.cmake
)
set_tests_properties(DiagnosticsCliRendersSourceContext PROPERTIES
        LABELS "diagnostics;type-checking"
)

add_test(NAME DiagnosticsCliRendersLexerSourceContext
        COMMAND ${CMAKE_COMMAND}
                -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                -DINPUT_FILE=${JOYEER_TESTS_DIR}/lexer/err/unterminated_comment.joyeer
                "-DEXPECTED_HEADER=unterminated_comment.joyeer:1:1: error[lexer.unterminated-comment]"
                "-DEXPECTED_SOURCE=/*asdfasdf"
                -P ${JOYEER_SOURCE_DIR}/scripts/cmake/expectRichCompilerDiagnostic.cmake
)
set_tests_properties(DiagnosticsCliRendersLexerSourceContext PROPERTIES
        LABELS "diagnostics;lexer"
)

add_test(NAME DiagnosticsCliSuggestsMissingDelimiterFix
        COMMAND ${CMAKE_COMMAND}
                -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                -DINPUT_FILE=${JOYEER_TESTS_DIR}/parser/err/missing_close_paren.joyeer
                "-DEXPECTED_HEADER=missing_close_paren.joyeer:3:1: error[parser.expected-token]"
                "-DEXPECTED_SOURCE=}"
                "-DEXPECTED_HELP=help: insert ')' before this token"
                "-DEXPECTED_FIX_IT=fix-it: missing_close_paren.joyeer:3:1:0: \")\""
                -P ${JOYEER_SOURCE_DIR}/scripts/cmake/expectRichCompilerDiagnostic.cmake
)
set_tests_properties(DiagnosticsCliSuggestsMissingDelimiterFix PROPERTIES
        LABELS "diagnostics;parser;fix-it"
)

add_test(NAME DiagnosticsCliSuggestsMatchArrowReplacement
        COMMAND ${CMAKE_COMMAND}
                -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                -DINPUT_FILE=${JOYEER_TESTS_DIR}/parser/err/wrong_match_arrow.joyeer
                "-DEXPECTED_HEADER=wrong_match_arrow.joyeer:3:14: error[parser.expected-token]"
                "-DEXPECTED_SOURCE=        true = 1,"
                "-DEXPECTED_HELP=help: replace '=' with '=>'"
                "-DEXPECTED_FIX_IT=fix-it: wrong_match_arrow.joyeer:3:14:1: \"=>\""
                -P ${JOYEER_SOURCE_DIR}/scripts/cmake/expectRichCompilerDiagnostic.cmake
)
set_tests_properties(DiagnosticsCliSuggestsMatchArrowReplacement PROPERTIES
        LABELS "diagnostics;parser;fix-it"
)

add_test(NAME DiagnosticsCliSuggestsEmptyPayloadDeletion
        COMMAND ${CMAKE_COMMAND}
                -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                -DINPUT_FILE=${JOYEER_TESTS_DIR}/parser/err/empty_enum_payload.joyeer
                "-DEXPECTED_HEADER=empty_enum_payload.joyeer:1:19: error[parser.missing-list-element]"
                "-DEXPECTED_SOURCE=enum Value { None() }"
                "-DEXPECTED_HELP=help: remove the empty payload clause"
                "-DEXPECTED_FIX_IT=fix-it: empty_enum_payload.joyeer:1:18:2: \"\""
                -P ${JOYEER_SOURCE_DIR}/scripts/cmake/expectRichCompilerDiagnostic.cmake
)
set_tests_properties(DiagnosticsCliSuggestsEmptyPayloadDeletion PROPERTIES
        LABELS "diagnostics;parser;fix-it"
)

add_test(NAME SourceFileCliRejectsDirectory
        COMMAND ${CMAKE_COMMAND}
                -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                -DINPUT_FILE=${JOYEER_TESTS_DIR}
                -DEXPECTED_PATTERN=source-file.read-failed
                -P ${JOYEER_SOURCE_DIR}/scripts/cmake/expectCompilerFailure.cmake
)
set_tests_properties(SourceFileCliRejectsDirectory PROPERTIES
        LABELS "diagnostics;lexer"
)

add_test(NAME SemanticAnalysisCliRejectsMissingReturn
        COMMAND ${CMAKE_COMMAND}
                -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                -DINPUT_FILE=${JOYEER_TESTS_DIR}/semantic-analysis/err/missing_return.joyeer
                -DEXPECTED_PATTERN=semantic-analysis.missing-return
                -P ${JOYEER_SOURCE_DIR}/scripts/cmake/expectCompilerFailure.cmake
)
set_tests_properties(SemanticAnalysisCliRejectsMissingReturn PROPERTIES
        LABELS semantic-analysis
)

add_test(NAME SemanticAnalysisCliRejectsUseBeforeInitialization
        COMMAND ${CMAKE_COMMAND}
                -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                -DINPUT_FILE=${JOYEER_TESTS_DIR}/semantic-analysis/err/use_before_initialization.joyeer
                -DEXPECTED_PATTERN=semantic-analysis.use-before-initialization
                -P ${JOYEER_SOURCE_DIR}/scripts/cmake/expectCompilerFailure.cmake
)
set_tests_properties(SemanticAnalysisCliRejectsUseBeforeInitialization PROPERTIES
        LABELS semantic-analysis
)

add_test(NAME SemanticAnalysisCliRejectsUseAfterConsume
        COMMAND ${CMAKE_COMMAND}
                -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                -DINPUT_FILE=${JOYEER_TESTS_DIR}/semantic-analysis/err/use_after_consume.joyeer
                -DEXPECTED_PATTERN=semantic-analysis.use-after-consume
                -P ${JOYEER_SOURCE_DIR}/scripts/cmake/expectCompilerFailure.cmake
)
set_tests_properties(SemanticAnalysisCliRejectsUseAfterConsume PROPERTIES
        LABELS "semantic-analysis;consuming"
)

add_test(NAME SemanticAnalysisCliRequiresInitializingOnAllPaths
        COMMAND ${CMAKE_COMMAND}
                -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                -DINPUT_FILE=${JOYEER_TESTS_DIR}/semantic-analysis/err/initializing_not_initialized.joyeer
                -DEXPECTED_PATTERN=semantic-analysis.initializing-parameter-not-initialized
                -P ${JOYEER_SOURCE_DIR}/scripts/cmake/expectCompilerFailure.cmake
)
set_tests_properties(SemanticAnalysisCliRequiresInitializingOnAllPaths PROPERTIES
        LABELS "semantic-analysis;initializing"
)

add_test(NAME SemanticAnalysisCliReportsUnreachableWarning
        COMMAND $<TARGET_FILE:joyeer>
                "${JOYEER_TESTS_DIR}/semantic-analysis/unreachable_warning.joyeer"
)
set_tests_properties(SemanticAnalysisCliReportsUnreachableWarning PROPERTIES
        LABELS semantic-analysis
        PASS_REGULAR_EXPRESSION semantic-analysis.unreachable-code
)

add_test(NAME SemanticAnalysisCliReportsUnusedBindingWarning
        COMMAND $<TARGET_FILE:joyeer>
                "${JOYEER_TESTS_DIR}/semantic-analysis/unused_warning.joyeer"
)
set_tests_properties(SemanticAnalysisCliReportsUnusedBindingWarning PROPERTIES
        LABELS semantic-analysis
        PASS_REGULAR_EXPRESSION semantic-analysis.unused-binding
)

add_test(NAME NativeLinkerRejectsMissingMain
        COMMAND ${CMAKE_COMMAND}
                -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                -DINPUT_FILE=${JOYEER_TESTS_DIR}/native/missing_main.joyeer
                -DEXPECTED_PATTERN=linker.missing-entry-point
                "-DEXTRA_ARGS=-o;${CMAKE_CURRENT_BINARY_DIR}/missing-main${CMAKE_EXECUTABLE_SUFFIX}"
                -DOUTPUT_TO_REMOVE=${CMAKE_CURRENT_BINARY_DIR}/missing-main${CMAKE_EXECUTABLE_SUFFIX}
                -P ${JOYEER_SOURCE_DIR}/scripts/cmake/expectCompilerFailure.cmake
)
set_tests_properties(NativeLinkerRejectsMissingMain PROPERTIES
        LABELS "native;llvm-backend"
)

add_test(NAME ParserMvpCliAcceptance
        COMMAND $<TARGET_FILE:joyeer>
                "${JOYEER_TESTS_DIR}/parser/ok/json_mvp.joyeer"
)
set_tests_properties(ParserMvpCliAcceptance PROPERTIES
        LABELS "parser;name-resolution;type-checking;ir-lowering"
)

add_test(NAME ParserMvpCliRejectsInvalidSource
        COMMAND ${CMAKE_COMMAND}
                -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                -DINPUT_FILE=${JOYEER_TESTS_DIR}/parser/err/missing_expression.joyeer
                -DEXPECTED_PATTERN=parser.expected-expression
                -P ${JOYEER_SOURCE_DIR}/scripts/cmake/expectCompilerFailure.cmake
)
set_tests_properties(ParserMvpCliRejectsInvalidSource PROPERTIES
        LABELS parser
)
