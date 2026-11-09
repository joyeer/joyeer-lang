add_executable(CompilerLexerTests
        lexer_test.cpp
)

add_executable(CompilerDiagnosticTests
        diagnostic_test.cpp
)

target_link_libraries(CompilerDiagnosticTests
        GTest::gtest_main
        JoyeerCompilerCore
)

joyeer_discover_tests(CompilerDiagnosticTests
        PROPERTIES LABELS diagnostics
)

target_link_libraries(CompilerLexerTests
        GTest::gtest_main
        JoyeerCompilerCore
)

joyeer_discover_tests(CompilerLexerTests
        PROPERTIES LABELS lexer
)

add_executable(CompilerParserTests
        parser_test.cpp
)

target_link_libraries(CompilerParserTests
        GTest::gtest_main
        JoyeerCompilerCore
)

target_compile_definitions(CompilerParserTests PRIVATE
        JOYEER_TESTS_DIR="${JOYEER_TESTS_DIR}"
)

joyeer_discover_tests(CompilerParserTests
        PROPERTIES LABELS parser
)

add_executable(CompilerNameResolutionTests
        nameresolution_test.cpp
        module_test.cpp
)

target_link_libraries(CompilerNameResolutionTests
        GTest::gtest_main
        JoyeerCompilerCore
)

target_compile_definitions(CompilerNameResolutionTests PRIVATE
        JOYEER_TESTS_DIR="${JOYEER_TESTS_DIR}"
)

joyeer_discover_tests(CompilerNameResolutionTests
        PROPERTIES LABELS name-resolution
)

add_executable(CompilerModuleServiceTests moduleservice.cpp)
target_link_libraries(CompilerModuleServiceTests PRIVATE
        GTest::gtest_main
        JoyeerCompilerCore
)
target_compile_definitions(CompilerModuleServiceTests PRIVATE
        JOYEER_LIBRARY_SOURCES="${JOYEER_SOURCE_DIR}/lib"
)
joyeer_discover_tests(CompilerModuleServiceTests
        PROPERTIES LABELS modules
)

add_executable(CompilerModuleArgumentsTests modulearguments_test.cpp)
target_link_libraries(CompilerModuleArgumentsTests PRIVATE
        GTest::gtest_main
        JoyeerCompilerCore
)
joyeer_discover_tests(CompilerModuleArgumentsTests
        PROPERTIES LABELS modules
)

add_executable(CompilerTypeCheckingTests
        typechecking_test.cpp
)

target_link_libraries(CompilerTypeCheckingTests
        GTest::gtest_main
        JoyeerCompilerCore
)

target_compile_definitions(CompilerTypeCheckingTests PRIVATE
        JOYEER_TESTS_DIR="${JOYEER_TESTS_DIR}"
)

joyeer_discover_tests(CompilerTypeCheckingTests
        PROPERTIES LABELS type-checking
)

add_executable(CompilerCInteropTests cinterop_test.cpp)
target_link_libraries(CompilerCInteropTests PRIVATE GTest::gtest_main JoyeerCompilerCore)
joyeer_discover_tests(CompilerCInteropTests
        PROPERTIES LABELS c-interop
)

add_executable(CompilerPlatformTests platform_test.cpp)
target_link_libraries(CompilerPlatformTests PRIVATE GTest::gtest_main JoyeerCompilerCore)
joyeer_discover_tests(CompilerPlatformTests
        PROPERTIES LABELS platform
)

add_executable(CompilerSemanticAnalysisTests
        semanticanalysis_test.cpp
)

target_link_libraries(CompilerSemanticAnalysisTests
        GTest::gtest_main
        JoyeerCompilerCore
)

joyeer_discover_tests(CompilerSemanticAnalysisTests
        PROPERTIES LABELS semantic-analysis
)

add_executable(IRModelTests
        ir_test.cpp
)

target_link_libraries(IRModelTests
        GTest::gtest_main
        JoyeerCompilerCore
)

joyeer_discover_tests(IRModelTests
        PROPERTIES LABELS ir
)

add_executable(IRLoweringTests
        irlowering_test.cpp
)

target_link_libraries(IRLoweringTests
        GTest::gtest_main
        JoyeerCompilerCore
)

target_compile_definitions(IRLoweringTests PRIVATE
        JOYEER_TESTS_DIR="${JOYEER_TESTS_DIR}"
)

joyeer_discover_tests(IRLoweringTests
        PROPERTIES LABELS ir-lowering
)

add_executable(LLVMBackendTests
        llvm_test.cpp
        hostbuiltins_test.cpp
)

target_link_libraries(LLVMBackendTests
        GTest::gtest_main
        JoyeerCompilerCore
)
target_compile_definitions(LLVMBackendTests PRIVATE
        JOYEER_TEST_RUNTIME_LIBRARY="$<TARGET_FILE:JoyeerRuntime>"
)
add_dependencies(LLVMBackendTests JoyeerRuntime)

joyeer_discover_tests(LLVMBackendTests
        PROPERTIES LABELS llvm-backend
)

add_executable(BackendAbiTests
        backend_abi_test.c
)
target_link_libraries(BackendAbiTests
        JoyeerBackend
)
add_test(NAME BackendAbiTests COMMAND BackendAbiTests)
set_tests_properties(BackendAbiTests PROPERTIES
        LABELS "llvm-backend;native;abi"
)

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
            JoyeerCompilerCore
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
                    -DINPUT_FILE=${JOYEER_TESTS_DIR}/e2e/hello.joyeer
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
                    -DINPUT_FILE=${JOYEER_TESTS_DIR}/e2e/hello.joyeer
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
                    -DINPUT_FILE=${JOYEER_TESTS_DIR}/e2e/full_debug.joyeer
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
                        -DINPUT_FILE=${JOYEER_TESTS_DIR}/e2e/hello.joyeer
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

    add_test(NAME E2EExecutableHello
            COMMAND ${CMAKE_COMMAND}
                    -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                    -DINPUT_FILE=${JOYEER_TESTS_DIR}/e2e/hello.joyeer
                    -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/joyeer-native-hello${CMAKE_EXECUTABLE_SUFFIX}
                    -DEXPECTED_OUTPUT=42\nnative\n8\n42\n
                    -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake
    )
    set_tests_properties(E2EExecutableHello PROPERTIES
            LABELS "e2e;llvm-backend;runtime"
    )

    set(module_sources
            "${JOYEER_TESTS_DIR}/modules/root/main.joyeer"
            "${JOYEER_TESTS_DIR}/modules/root/implementation files/helper.joyeer"
    )
    set(module_dependency_args
            --module-source "project.config=${JOYEER_TESTS_DIR}/modules/config/entry.joyeer"
            --module-source "project.config=${JOYEER_TESTS_DIR}/modules/config/types/record.joyeer"
    )
    foreach(level IN ITEMS 0 1 2 3)
        add_test(NAME E2EExecutableCompilationUnitsO${level}
                COMMAND ${CMAKE_COMMAND}
                        -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                        -DMODULE_NAME=project.app
                        "-DINPUT_FILES=${module_sources}"
                        "-DCOMPILER_ARGS=-O${level};${module_dependency_args}"
                        -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/joyeer-native-modules-O${level}${CMAKE_EXECUTABLE_SUFFIX}
                        -DEXPECTED_OUTPUT=42\n
                        -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake
        )
        set_tests_properties(E2EExecutableCompilationUnitsO${level} PROPERTIES
                LABELS "modules;e2e;llvm-backend"
        )
        add_test(NAME E2EExecutableUnqualifiedImportsO${level}
                COMMAND ${CMAKE_COMMAND}
                        -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                        -DMODULE_NAME=project.app
                        "-DINPUT_FILES=${JOYEER_TESTS_DIR}/modules/unqualified/main.joyeer"
                        "-DCOMPILER_ARGS=-O${level};--module-source;project.model=${JOYEER_TESTS_DIR}/modules/unqualified/model.joyeer;--module-source;project.other=${JOYEER_TESTS_DIR}/modules/unqualified/other.joyeer"
                        -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/joyeer-native-unqualified-O${level}${CMAKE_EXECUTABLE_SUFFIX}
                        -DEXPECTED_OUTPUT=42\n42\n42\n42\nowned\n
                        -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake
        )
        set_tests_properties(E2EExecutableUnqualifiedImportsO${level} PROPERTIES
                LABELS "modules;e2e;llvm-backend;ownership"
        )
    endforeach()

    if(WIN32 AND JOYEER_LLVM_READOBJ_EXECUTABLE)
        add_test(NAME E2EExecutableArchitecture
                COMMAND ${CMAKE_COMMAND}
                        -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                        -DINPUT_FILE=${JOYEER_TESTS_DIR}/e2e/hello.joyeer
                        -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/joyeer-native-architecture${CMAKE_EXECUTABLE_SUFFIX}
                        -DLLVM_READOBJ=${JOYEER_LLVM_READOBJ_EXECUTABLE}
                        -DEXPECTED_MACHINE=${JOYEER_WINDOWS_MACHINE}
                        -DEXPECTED_OUTPUT=42\nnative\n8\n42\n
                        -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake
        )
        set_tests_properties(E2EExecutableArchitecture PROPERTIES
                LABELS "e2e;llvm-backend;runtime;architecture"
        )
    endif()

    add_test(NAME InstalledCompilerNativeHello
            COMMAND ${CMAKE_COMMAND}
                    -DBUILD_DIR=${CMAKE_BINARY_DIR}
                    -DSTAGING_DIR=${CMAKE_CURRENT_BINARY_DIR}/installed-compiler
                    -DRUNTIME_ARCHIVE_NAME=${CMAKE_STATIC_LIBRARY_PREFIX}JoyeerRuntime${CMAKE_STATIC_LIBRARY_SUFFIX}
                    -DINPUT_FILE=${JOYEER_TESTS_DIR}/e2e/hello.joyeer
                    -DEXECUTABLE_SUFFIX=${CMAKE_EXECUTABLE_SUFFIX}
                    -DEXPECTED_OUTPUT=42\nnative\n8\n42\n
                    -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyInstalledCompiler.cmake
    )
    set_tests_properties(InstalledCompilerNativeHello PROPERTIES
            LABELS "e2e;llvm-backend;runtime;packaging"
    )

    add_test(NAME E2EExecutableNoDebugArtifact
            COMMAND ${CMAKE_COMMAND}
                    -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                    -DINPUT_FILE=${JOYEER_TESTS_DIR}/e2e/hello.joyeer
                    -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/joyeer-native-no-debug${CMAKE_EXECUTABLE_SUFFIX}
                    "-DCOMPILER_ARGS=-O0;-g0"
                    -DEXPECT_NO_DEBUG_ARTIFACT=ON
                    -DEXPECTED_OUTPUT=42\nnative\n8\n42\n
                    -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake
    )
    set_tests_properties(E2EExecutableNoDebugArtifact PROPERTIES
            LABELS "e2e;llvm-backend;runtime;debug-info;optimization"
    )

    add_test(NAME E2EExecutableMetacharacterPath
            COMMAND ${CMAKE_COMMAND}
                    -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                    -DINPUT_FILE=${JOYEER_TESTS_DIR}/e2e/hello.joyeer
                    "-DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/joyeer native $() %PATH% & safe${CMAKE_EXECUTABLE_SUFFIX}"
                    -DEXPECT_NO_DEBUG_ARTIFACT=ON
                    -DEXPECTED_OUTPUT=42\nnative\n8\n42\n
                    -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake
    )
    set_tests_properties(E2EExecutableMetacharacterPath PROPERTIES
            LABELS "e2e;llvm-backend;runtime;security"
    )

    add_test(NAME E2ELinkerPreservesOutputDirectory
            COMMAND ${CMAKE_COMMAND}
                    -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                    -DINPUT_FILE=${JOYEER_TESTS_DIR}/e2e/hello.joyeer
                    -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/joyeer-protected-output
                    -DARTIFACT_KIND=OUTPUT
                    -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeArtifactDirectoryRejected.cmake
    )
    set_tests_properties(E2ELinkerPreservesOutputDirectory PROPERTIES
            LABELS "e2e;llvm-backend;security"
    )

    add_test(NAME E2ELinkerRejectsSourceOutputCollision
            COMMAND ${CMAKE_COMMAND}
                    -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                    -DSOURCE_TEMPLATE=${JOYEER_TESTS_DIR}/e2e/hello.joyeer
                    -DTEST_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/source-output-collision
                    -DCOLLISION_KIND=OUTPUT
                    -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifySourceOutputCollisionRejected.cmake
    )
    set_tests_properties(E2ELinkerRejectsSourceOutputCollision PROPERTIES
            LABELS "e2e;llvm-backend;security"
    )

    add_test(NAME LLVMEmitterRejectsSourceOutputCollision
            COMMAND ${CMAKE_COMMAND}
                    -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                    -DSOURCE_TEMPLATE=${JOYEER_TESTS_DIR}/e2e/hello.joyeer
                    -DTEST_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/source-llvm-collision
                    -DCOLLISION_KIND=LLVM
                    -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifySourceOutputCollisionRejected.cmake
    )
    set_tests_properties(LLVMEmitterRejectsSourceOutputCollision PROPERTIES
            LABELS "llvm-backend;security"
    )

        if(WIN32 AND JOYEER_LLVM_READOBJ_EXECUTABLE AND JOYEER_LLVM_PDBUTIL_EXECUTABLE)
        add_test(NAME E2EExecutableCompilationUnitsDebug
                COMMAND ${CMAKE_COMMAND}
                        -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                        -DMODULE_NAME=project.app
                        "-DINPUT_FILES=${module_sources}"
                        "-DCOMPILER_ARGS=-O0;-gfull;-gcodeview;${module_dependency_args}"
                        -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/joyeer-native-modules-debug${CMAKE_EXECUTABLE_SUFFIX}
                        -DEXPECT_PDB=ON
                        -DLLVM_PDBUTIL=${JOYEER_LLVM_PDBUTIL_EXECUTABLE}
                        "-DEXPECTED_PDB_PATTERNS=main.joyeer;helper.joyeer;record.joyeer;Lines"
                        -DEXPECTED_OUTPUT=42\n
                        -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake
        )
        set_tests_properties(E2EExecutableCompilationUnitsDebug PROPERTIES
                LABELS "modules;e2e;llvm-backend;debug-info"
        )

        add_test(NAME E2EExecutableCodeViewPdb
                COMMAND ${CMAKE_COMMAND}
                        -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                        -DINPUT_FILE=${JOYEER_TESTS_DIR}/e2e/hello.joyeer
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
        set_tests_properties(E2EExecutableCodeViewPdb PROPERTIES
                LABELS "e2e;llvm-backend;runtime;debug-info"
        )

        add_test(NAME E2EExecutableFullDebugPdb
                COMMAND ${CMAKE_COMMAND}
                        -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                        -DINPUT_FILE=${JOYEER_TESTS_DIR}/e2e/full_debug.joyeer
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
        set_tests_properties(E2EExecutableFullDebugPdb PROPERTIES
                LABELS "e2e;llvm-backend;runtime;debug-info"
        )

        add_test(NAME E2ELinkerPreservesPdbDirectory
                COMMAND ${CMAKE_COMMAND}
                        -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                        -DINPUT_FILE=${JOYEER_TESTS_DIR}/e2e/hello.joyeer
                        -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/joyeer-protected-pdb.exe
                        -DARTIFACT_KIND=PDB
                        -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeArtifactDirectoryRejected.cmake
        )
        set_tests_properties(E2ELinkerPreservesPdbDirectory PROPERTIES
                LABELS "e2e;llvm-backend;debug-info;security"
        )

        add_test(NAME E2ELinkerRejectsSourcePdbCollision
                COMMAND ${CMAKE_COMMAND}
                        -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                        -DSOURCE_TEMPLATE=${JOYEER_TESTS_DIR}/e2e/hello.joyeer
                        -DTEST_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/source-pdb-collision
                        -DCOLLISION_KIND=PDB
                        -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifySourceOutputCollisionRejected.cmake
        )
        set_tests_properties(E2ELinkerRejectsSourcePdbCollision PROPERTIES
                LABELS "e2e;llvm-backend;debug-info;security"
        )

        add_test(NAME E2ELinkerRejectsNoDebugSourcePdbCollision
                COMMAND ${CMAKE_COMMAND}
                        -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                        -DSOURCE_TEMPLATE=${JOYEER_TESTS_DIR}/e2e/hello.joyeer
                        -DTEST_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/source-nodebug-pdb-collision
                        -DCOLLISION_KIND=PDB_NODEBUG
                        -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifySourceOutputCollisionRejected.cmake
        )
        set_tests_properties(E2ELinkerRejectsNoDebugSourcePdbCollision PROPERTIES
                LABELS "e2e;llvm-backend;security"
        )

        add_test(NAME E2ELinkerRemovesStalePdb
                COMMAND ${CMAKE_COMMAND}
                        -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                        -DINPUT_FILE=${JOYEER_TESTS_DIR}/e2e/hello.joyeer
                        -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/joyeer-debug-transition.exe
                        -DARTIFACT_KIND=PDB
                        -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeDebugArtifactTransition.cmake
        )
        set_tests_properties(E2ELinkerRemovesStalePdb PROPERTIES
                LABELS "e2e;llvm-backend;debug-info"
        )

        add_test(NAME E2ELinkerRejectsPdbOutputCollision
                COMMAND ${CMAKE_COMMAND}
                        -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                        -DINPUT_FILE=${JOYEER_TESTS_DIR}/e2e/hello.joyeer
                        -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/joyeer-output-collision.PDB
                        -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativePdbCollisionRejected.cmake
        )
        set_tests_properties(E2ELinkerRejectsPdbOutputCollision PROPERTIES
                LABELS "e2e;llvm-backend;debug-info;security"
        )

        add_test(NAME E2ELinkerRejectsTrailingSpacePdbAlias
                COMMAND ${CMAKE_COMMAND}
                        -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                        -DINPUT_FILE=${JOYEER_TESTS_DIR}/e2e/hello.joyeer
                        "-DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/joyeer-output-alias.pdb "
                        -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativePdbCollisionRejected.cmake
        )
        set_tests_properties(E2ELinkerRejectsTrailingSpacePdbAlias PROPERTIES
                LABELS "e2e;llvm-backend;debug-info;security"
        )

        add_test(NAME E2ELinkerRejectsAlternateDataStreamOutput
                COMMAND ${CMAKE_COMMAND}
                        -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                        -DINPUT_FILE=${JOYEER_TESTS_DIR}/e2e/hello.joyeer
                        "-DEXTRA_ARGS=-o;${CMAKE_CURRENT_BINARY_DIR}/joyeer-output.exe:stream"
                        "-DEXPECTED_PATTERN=alternate data streams"
                        -P ${JOYEER_SOURCE_DIR}/scripts/cmake/expectCompilerFailure.cmake
        )
        set_tests_properties(E2ELinkerRejectsAlternateDataStreamOutput PROPERTIES
                LABELS "e2e;llvm-backend;security"
        )
        endif()

        if(WIN32 AND JOYEER_LLVM_READOBJ_EXECUTABLE)
        add_test(NAME E2EExecutableDwarfArtifact
                COMMAND ${CMAKE_COMMAND}
                        -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                        -DINPUT_FILE=${JOYEER_TESTS_DIR}/e2e/hello.joyeer
                        -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/joyeer-native-dwarf${CMAKE_EXECUTABLE_SUFFIX}
                        "-DCOMPILER_ARGS=-O2;-gdwarf"
                        -DLLVM_READOBJ=${JOYEER_LLVM_READOBJ_EXECUTABLE}
                        "-DEXPECTED_DEBUG_PATTERNS=.debug_info;.debug_line"
                        -DEXPECTED_OUTPUT=42\nnative\n8\n42\n
                        -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake
        )
        set_tests_properties(E2EExecutableDwarfArtifact PROPERTIES
                LABELS "e2e;llvm-backend;runtime;debug-info;optimization"
        )
        elseif(APPLE AND JOYEER_LLVM_READOBJ_EXECUTABLE AND JOYEER_DSYMUTIL_EXECUTABLE)
        add_test(NAME E2EExecutableDwarfArtifact
                COMMAND ${CMAKE_COMMAND}
                        -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                        -DINPUT_FILE=${JOYEER_TESTS_DIR}/e2e/hello.joyeer
                        -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/joyeer-native-dwarf${CMAKE_EXECUTABLE_SUFFIX}
                        "-DCOMPILER_ARGS=-O2;-gdwarf"
                        -DEXPECT_DSYM=ON
                        -DLLVM_READOBJ=${JOYEER_LLVM_READOBJ_EXECUTABLE}
                        -DEXPECTED_DEBUG_PATTERNS=__debug_line
                        -DEXPECTED_OUTPUT=42\nnative\n8\n42\n
                        -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake
        )
        set_tests_properties(E2EExecutableDwarfArtifact PROPERTIES
                LABELS "e2e;llvm-backend;runtime;debug-info;optimization"
        )

        add_test(NAME E2ELinkerRemovesStaleDsym
                COMMAND ${CMAKE_COMMAND}
                        -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                        -DINPUT_FILE=${JOYEER_TESTS_DIR}/e2e/hello.joyeer
                        -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/joyeer-debug-transition
                        -DARTIFACT_KIND=DSYM
                        -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeDebugArtifactTransition.cmake
        )
        set_tests_properties(E2ELinkerRemovesStaleDsym PROPERTIES
                LABELS "e2e;llvm-backend;debug-info"
        )
        elseif(UNIX AND NOT APPLE AND JOYEER_LLVM_READOBJ_EXECUTABLE)
        add_test(NAME E2EExecutableDwarfArtifact
                COMMAND ${CMAKE_COMMAND}
                        -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                        -DINPUT_FILE=${JOYEER_TESTS_DIR}/e2e/hello.joyeer
                        -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/joyeer-native-dwarf${CMAKE_EXECUTABLE_SUFFIX}
                        "-DCOMPILER_ARGS=-O2;-gdwarf"
                        -DLLVM_READOBJ=${JOYEER_LLVM_READOBJ_EXECUTABLE}
                        -DEXPECTED_DEBUG_PATTERNS=.debug_line
                        -DEXPECTED_OUTPUT=42\nnative\n8\n42\n
                        -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake
        )
        set_tests_properties(E2EExecutableDwarfArtifact PROPERTIES
                LABELS "e2e;llvm-backend;runtime;debug-info;optimization"
        )
    endif()

    add_test(NAME E2EExecutableOwnership
            COMMAND ${CMAKE_COMMAND}
                    -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                    -DINPUT_FILE=${JOYEER_TESTS_DIR}/e2e/ownership.joyeer
                    -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/joyeer-native-ownership${CMAKE_EXECUTABLE_SUFFIX}
                    -DEXPECTED_OUTPUT=left!\nleft!\nbeta\nvalue\nsame\n
                    -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake
    )
    set_tests_properties(E2EExecutableOwnership PROPERTIES
            LABELS "e2e;llvm-backend;runtime;ownership"
    )

    add_test(NAME E2EExecutableTrailingReturn
            COMMAND ${CMAKE_COMMAND}
                    -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                    -DINPUT_FILE=${JOYEER_TESTS_DIR}/e2e/trailing_return.joyeer
                    -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/joyeer-native-trailing-return${CMAKE_EXECUTABLE_SUFFIX}
                    -DEXPECTED_OUTPUT=42\ntrailing\n
                    -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake
    )
    set_tests_properties(E2EExecutableTrailingReturn PROPERTIES
            LABELS "e2e;llvm-backend;runtime;semantic-analysis"
    )

    function(joyeer_add_entry_case name fixture expected_output expected_exit)
        add_test(NAME ${name}
                COMMAND ${CMAKE_COMMAND}
                        -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                        -DINPUT_FILE=${JOYEER_TESTS_DIR}/e2e/${fixture}.joyeer
                        -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/${name}${CMAKE_EXECUTABLE_SUFFIX}
                        "-DEXPECTED_OUTPUT=${expected_output}"
                        -DEXPECTED_EXIT_CODE=${expected_exit}
                        ${ARGN}
                        -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake
        )
        set_tests_properties(${name} PROPERTIES
                LABELS "e2e;llvm-backend;runtime;entry"
                TIMEOUT 60)
    endfunction()

    joyeer_add_entry_case(E2EEntryNoArguments entry_arguments "0\n" 0)
    joyeer_add_entry_case(E2EEntryEmptyAndWhitespace entry_arguments
            "2\n0\n\n5\n a b \n" 73
            "-DPROGRAM_ARGUMENT_1=" "-DPROGRAM_ARGUMENT_2=a b"
            -DPROGRAM_ARGUMENT_2_SURROUND_SPACES=ON)
    string(ASCII 195 169 entry_accent)
    joyeer_add_entry_case(E2EEntryUnicodeArgument entry_arguments
            "2\n0\n\n3\nh${entry_accent}\n" 73
            "-DPROGRAM_ARGUMENT_1=" "-DPROGRAM_ARGUMENT_2=h${entry_accent}")
    joyeer_add_entry_case(E2EEntryUnoptimizedArguments entry_arguments
            "2\n0\n\n3\na b\n" 73
            "-DPROGRAM_ARGUMENT_1=" "-DPROGRAM_ARGUMENT_2=a b"
            -DCOMPILER_ARGS=-O0)
    # Quotes and backslashes survive the trip through the command line (the rules of the C
    # runtime and CommandLineToArgvW, which the Windows entry point implements itself).
    joyeer_add_entry_case(E2EEntryQuotesAndBackslashes entry_arguments
            "2\n8\nsay \"hi\"\n7\nC:\\dir\\\n" 73
            "-DPROGRAM_ARGUMENT_1=say \"hi\"" "-DPROGRAM_ARGUMENT_2=C:\\dir\\")
    joyeer_add_entry_case(E2EEntryLegacyIgnoresArguments hello
            "42\nnative\n8\n42\n" 0
            "-DPROGRAM_ARGUMENT_1=" "-DPROGRAM_ARGUMENT_2=ignored")
    if(UNIX)
        joyeer_add_entry_case(E2EEntryPosixRawBytes entry_argument_bytes
                "1\n1\n255\n" 0
                -DPROGRAM_ARGUMENT_1_NON_UTF8=ON)
    endif()
    joyeer_add_entry_case(E2EEntryExitZero entry_exit_status "cleanup\n" 0)
    joyeer_add_entry_case(E2EEntryExitOne entry_exit_status "cleanup\n" 1
            -DPROGRAM_ARGUMENT_1=one)
    joyeer_add_entry_case(E2EEntryExitMaximum entry_exit_status "cleanup\n" 255
            -DPROGRAM_ARGUMENT_1=max)
    joyeer_add_entry_case(E2EEntryExitNegative entry_exit_status "cleanup\n" 1
            -DPROGRAM_ARGUMENT_1=negative
            "-DEXPECTED_ERROR_PATTERN=entry returned exit code -1 outside 0")
    joyeer_add_entry_case(E2EEntryExitOverflow entry_exit_status "cleanup\n" 1
            -DPROGRAM_ARGUMENT_1=overflow
            "-DEXPECTED_ERROR_PATTERN=entry returned exit code 256 outside 0")

    add_test(NAME E2EEntryRejectsInvalidSignature
            COMMAND ${CMAKE_COMMAND}
                    -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                    -DINPUT_FILE=${JOYEER_TESTS_DIR}/e2e/err/invalid_entry_signature.joyeer
                    "-DEXTRA_ARGS=-o;${CMAKE_CURRENT_BINARY_DIR}/invalid-entry${CMAKE_EXECUTABLE_SUFFIX}"
                    -DEXPECTED_PATTERN=llvm.invalid-entry-point
                    -P ${JOYEER_SOURCE_DIR}/scripts/cmake/expectCompilerFailure.cmake
    )
    set_tests_properties(E2EEntryRejectsInvalidSignature PROPERTIES
            LABELS "e2e;llvm-backend;entry;diagnostics")

    add_test(NAME E2EEntryRejectsDuplicate
            COMMAND ${CMAKE_COMMAND}
                    -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                    -DINPUT_FILE=${JOYEER_TESTS_DIR}/e2e/err/duplicate_entry.joyeer
                    "-DEXTRA_ARGS=-o;${CMAKE_CURRENT_BINARY_DIR}/duplicate-entry${CMAKE_EXECUTABLE_SUFFIX}"
                    -DEXPECTED_PATTERN=name-resolution.duplicate-declaration
                    -P ${JOYEER_SOURCE_DIR}/scripts/cmake/expectCompilerFailure.cmake
    )
    set_tests_properties(E2EEntryRejectsDuplicate PROPERTIES
            LABELS "e2e;name-resolution;entry;diagnostics")

    add_test(NAME E2EEntryRejectsMissing
            COMMAND ${CMAKE_COMMAND}
                    -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                    -DINPUT_FILE=${JOYEER_TESTS_DIR}/e2e/err/missing_entry.joyeer
                    "-DEXTRA_ARGS=-o;${CMAKE_CURRENT_BINARY_DIR}/missing-entry${CMAKE_EXECUTABLE_SUFFIX}"
                    -DEXPECTED_PATTERN=linker.missing-entry-point
                    -P ${JOYEER_SOURCE_DIR}/scripts/cmake/expectCompilerFailure.cmake
    )
    set_tests_properties(E2EEntryRejectsMissing PROPERTIES
            LABELS "e2e;llvm-backend;entry;diagnostics")

    add_test(NAME E2EExecutableDeferredInitialization
            COMMAND ${CMAKE_COMMAND}
                    -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                    -DINPUT_FILE=${JOYEER_TESTS_DIR}/e2e/deferred_initialization.joyeer
                    -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/joyeer-native-deferred-initialization${CMAKE_EXECUTABLE_SUFFIX}
                    -DEXPECTED_OUTPUT=42\ninitialized\n
                    -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake
    )
    set_tests_properties(E2EExecutableDeferredInitialization PROPERTIES
            LABELS "e2e;llvm-backend;runtime;semantic-analysis;ownership"
    )

    add_test(NAME E2EExecutableReadFile
            COMMAND ${CMAKE_COMMAND}
                    -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                    -DINPUT_FILE=${JOYEER_TESTS_DIR}/e2e/read_file.joyeer
                    -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/joyeer-native-read-file${CMAKE_EXECUTABLE_SUFFIX}
                    "-DEXPECTED_OUTPUT=file contents\nnot found\ntrue\n"
                    -DPROGRAM_WORKING_DIRECTORY=${JOYEER_TESTS_DIR}/e2e
                    -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake
    )
    set_tests_properties(E2EExecutableReadFile PROPERTIES
            LABELS "e2e;llvm-backend;runtime;ownership;file-io"
    )

    foreach(level IN ITEMS 0 2)
        add_test(NAME E2EExecutableStdinLinesO${level}
                COMMAND ${CMAKE_COMMAND}
                        -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                        -DINPUT_FILE=${JOYEER_TESTS_DIR}/e2e/stdin_lines.joyeer
                        -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/joyeer-native-stdin-lines-O${level}${CMAKE_EXECUTABLE_SUFFIX}
                        -DCOMPILER_ARGS=-O${level}
                        -DPROGRAM_STDIN_FILE=${JOYEER_TESTS_DIR}/e2e/stdin_lines_input.txt
                        "-DEXPECTED_OUTPUT=[alpha]\ntoo long\n[]\ntoo long\n[ok]\n[last]\nend\n"
                        -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake
        )
        set_tests_properties(E2EExecutableStdinLinesO${level} PROPERTIES
                LABELS "e2e;llvm-backend;runtime;ownership;stdin"
                TIMEOUT 60
        )
    endforeach()

    add_test(NAME E2EExecutableArrayAppend
            COMMAND ${CMAKE_COMMAND}
                    -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                    -DINPUT_FILE=${JOYEER_TESTS_DIR}/e2e/array_append.joyeer
                    -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/joyeer-native-array-append${CMAKE_EXECUTABLE_SUFFIX}
                    -DEXPECTED_OUTPUT=3\n30\nfirst!\nsecond\nsecond\n2\n6\n
                    -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake
    )
    set_tests_properties(E2EExecutableArrayAppend PROPERTIES
            LABELS "e2e;llvm-backend;runtime;ownership;array"
    )

    set(dictionary_set_output
            "3\nupdated!\ntwo\nretained\nretained\n2\n6\n2\n40\n30\n2\n40\n30\n2\nlast\nkept\n2\nlast\nkept\n")
    add_test(NAME E2EExecutableDictionarySet
            COMMAND ${CMAKE_COMMAND}
                    -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                    -DINPUT_FILE=${JOYEER_TESTS_DIR}/e2e/dictionary_set.joyeer
                    -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/joyeer-native-dictionary-set${CMAKE_EXECUTABLE_SUFFIX}
                    "-DEXPECTED_OUTPUT=${dictionary_set_output}"
                    -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake
    )
    set_tests_properties(E2EExecutableDictionarySet PROPERTIES
            LABELS "e2e;llvm-backend;runtime;ownership;dictionary"
    )

    add_test(NAME E2EExecutableByteConversion
            COMMAND ${CMAKE_COMMAND}
                    -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                    -DINPUT_FILE=${JOYEER_TESTS_DIR}/e2e/byte_conversion.joyeer
                    -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/joyeer-native-byte-conversion${CMAKE_EXECUTABLE_SUFFIX}
                    -DEXPECTED_OUTPUT=7\nJ\nJoy\n
                    -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake
    )
    set_tests_properties(E2EExecutableByteConversion PROPERTIES
            LABELS "e2e;llvm-backend;runtime;ownership;byte-conversion"
    )

    # The compiler targets its host, so each host expects its own variants.
    if(WIN32)
        set(platform_selection_output "windows\n\\\n")
    elseif(APPLE)
        set(platform_selection_output "macos\n/\n")
    else()
        set(platform_selection_output "linux\n/\n")
    endif()
    add_test(NAME E2EExecutablePlatformSelection
            COMMAND ${CMAKE_COMMAND}
                    -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                    -DINPUT_FILE=${JOYEER_TESTS_DIR}/e2e/platform_selection.joyeer
                    -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/joyeer-native-platform-selection${CMAKE_EXECUTABLE_SUFFIX}
                    "-DEXPECTED_OUTPUT=${platform_selection_output}"
                    -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake
    )
    set_tests_properties(E2EExecutablePlatformSelection PROPERTIES
            LABELS "e2e;llvm-backend;platform"
    )

    add_test(NAME E2EExecutableStringUtf8
            COMMAND ${CMAKE_COMMAND}
                    -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                    -DINPUT_FILE=${JOYEER_TESTS_DIR}/e2e/string_utf8.joyeer
                    -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/joyeer-native-string-utf8${CMAKE_EXECUTABLE_SUFFIX}
                    -DEXPECTED_OUTPUT=Joy\n3\n66\n111\n121\n0\n3\n104\n195\n169\n
                    -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake
    )
    set_tests_properties(E2EExecutableStringUtf8 PROPERTIES
            LABELS "e2e;llvm-backend;runtime;ownership;string;utf8"
    )

    add_test(NAME E2EExecutableStringIndex
            COMMAND ${CMAKE_COMMAND}
                    -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                    -DINPUT_FILE=${JOYEER_TESTS_DIR}/e2e/string_index.joyeer
                    -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/joyeer-native-string-index${CMAKE_EXECUTABLE_SUFFIX}
                    -DEXPECTED_OUTPUT=5\n97\n0\n195\n122\n
                    -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake
    )
    set_tests_properties(E2EExecutableStringIndex PROPERTIES
            LABELS "e2e;llvm-backend;runtime;string"
    )

    add_test(NAME E2EExecutableJsonParser
            COMMAND ${CMAKE_COMMAND}
                    -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                    -DINPUT_FILE=${JOYEER_TESTS_DIR}/e2e/json_parser.joyeer
                    -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/joyeer-native-json-parser${CMAKE_EXECUTABLE_SUFFIX}
                    "-DEXPECTED_OUTPUT=true\njoyeer\ntrue\n4\n10\nok\ntrue\n-7\nline\nquote: \"ok\"\ntrue\n"
                    -DPROGRAM_WORKING_DIRECTORY=${JOYEER_TESTS_DIR}/e2e
                    -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake
    )
    set_tests_properties(E2EExecutableJsonParser PROPERTIES
            LABELS "e2e;llvm-backend;runtime;ownership;file-io;array;dictionary;byte-conversion;json-parser"
    )

    add_test(NAME E2EExecutableConsuming
            COMMAND ${CMAKE_COMMAND}
                    -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                    -DINPUT_FILE=${JOYEER_TESTS_DIR}/e2e/consuming.joyeer
                    -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/joyeer-native-consuming${CMAKE_EXECUTABLE_SUFFIX}
                    -DEXPECTED_OUTPUT=first!\nsecond\nsecond\ntemporary!\nforwarded!\n2\nreinitialized\n
                    -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake
    )
    set_tests_properties(E2EExecutableConsuming PROPERTIES
            LABELS "e2e;llvm-backend;runtime;ownership;consuming"
    )

    add_test(NAME E2EExecutableInitializing
            COMMAND ${CMAKE_COMMAND}
                    -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                    -DINPUT_FILE=${JOYEER_TESTS_DIR}/e2e/initializing.joyeer
                    -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/joyeer-native-initializing${CMAKE_EXECUTABLE_SUFFIX}
                    -DEXPECTED_OUTPUT=created!\ncreated!\nforwarded!\n2\nfirst\nsecond\n
                    -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake
    )
    set_tests_properties(E2EExecutableInitializing PROPERTIES
            LABELS "e2e;llvm-backend;runtime;ownership;initializing"
    )

    add_test(NAME E2EExecutableProjectionConsume
            COMMAND ${CMAKE_COMMAND}
                    -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                    -DINPUT_FILE=${JOYEER_TESTS_DIR}/e2e/projection_consume.joyeer
                    -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/joyeer-native-projection-consume${CMAKE_EXECUTABLE_SUFFIX}
                    -DEXPECTED_OUTPUT=left\nright\nnew-left\nfirst\nnew-first\nsecond\nold\nnew\n
                    -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake
    )
    set_tests_properties(E2EExecutableProjectionConsume PROPERTIES
            LABELS "e2e;llvm-backend;runtime;ownership;consuming;projection-consume"
    )

    add_test(NAME E2EExecutableDeinit
            COMMAND ${CMAKE_COMMAND}
                    -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                    -DINPUT_FILE=${JOYEER_TESTS_DIR}/e2e/deinit.joyeer
                    -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/joyeer-native-deinit${CMAKE_EXECUTABLE_SUFFIX}
                    -DEXPECTED_OUTPUT=10\n2\n1\n20\n100\n3\n100\n4\n200\n200\n5\n6\n30\n40\n8\n7\n
                    -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake
    )
    set_tests_properties(E2EExecutableDeinit PROPERTIES
            LABELS "e2e;llvm-backend;runtime;ownership;consuming;deinit"
    )

    # The destroy helper that runs a `deinit` has no debug scope of its own; at -O1 and above
    # with full debug info the optimizer crashed until the helper was kept out of line.
    foreach(level 0 1 2)
        add_test(NAME E2EExecutableDeinitFullDebugO${level}
                COMMAND ${CMAKE_COMMAND}
                        -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                        -DINPUT_FILE=${JOYEER_TESTS_DIR}/e2e/deinit.joyeer
                        -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/joyeer-native-deinit-gfull-O${level}${CMAKE_EXECUTABLE_SUFFIX}
                        "-DCOMPILER_ARGS=-O${level};-gfull"
                        -DEXPECTED_OUTPUT=10\n2\n1\n20\n100\n3\n100\n4\n200\n200\n5\n6\n30\n40\n8\n7\n
                        -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake
        )
        set_tests_properties(E2EExecutableDeinitFullDebugO${level} PROPERTIES
                LABELS "e2e;llvm-backend;runtime;ownership;deinit;debug"
        )
    endforeach()

    function(add_joyeer_native_regression name fixture expected_output)
        set(levels 0 2)
        if(ARGC GREATER 3)
            set(levels ${ARGN})
        endif()
        foreach(level IN LISTS levels)
            add_test(NAME ${name}O${level}
                    COMMAND ${CMAKE_COMMAND}
                            -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                            -DINPUT_FILE=${JOYEER_TESTS_DIR}/e2e/${fixture}.joyeer
                            -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/${fixture}-O${level}${CMAKE_EXECUTABLE_SUFFIX}
                            -DCOMPILER_ARGS=-O${level}
                            "-DEXPECTED_OUTPUT=${expected_output}"
                            -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake
            )
            set_tests_properties(${name}O${level} PROPERTIES
                    LABELS "e2e;llvm-backend;runtime;ownership;safety;regression"
                    TIMEOUT 60
            )
        endforeach()
    endfunction()

    add_joyeer_native_regression(
            E2EDeinitMoves deinit_moves
            "10\n20\n30\n40\n5\n50\n60\n7\n6\n4\n3\n2\n1\n")
    add_joyeer_native_regression(
            E2EArrayRemoveLast array_remove_last
            "3\n2\n-1\nbb\n1\n2\n3\n2\n1\n")
    add_joyeer_native_regression(
            E2EMatchMoves match_moves
            "1\n1\n9\n9\n20\n30\n3\n41\n2\n")
    add_joyeer_native_regression(
            E2EMatchMovesOnward match_moves_onward
            "100\n1\n1\nname\n3\n2\n")
    add_joyeer_native_regression(
            E2ERegressionShortCircuit regression_lowering_short_circuit
            "false\n0\ntrue\n1\nfalse\nfalse\ntrue\nfalse\ntrue\nfalse\n")
    add_joyeer_native_regression(
            E2ERegressionCapture regression_lowering_capture
            "true\nreplacement\ntrue\nbefore\nafter\nafter\nafter\n")
    add_joyeer_native_regression(
            E2ERegressionPendingOperands regression_lowering_pending_operands
            "ab\n7\nab\nc\nvalue\nab\n42\nbeforebefore\ntrue\nfalse\nearly\nbefore!\npayload\npayload\nnested\narray\nnested\narray\n")
    add_joyeer_native_regression(
            E2ERegressionDictionaryDuplicates dictionary_set "${dictionary_set_output}" 0)
    add_joyeer_native_regression(
            E2ERegressionPatterns regression_backend_patterns
            "30\n10\n20\n3\n1\n2\n4\n")
    add_joyeer_native_regression(
            E2ERegressionLoopStack regression_backend_loop_stack
            "100000\n")
    add_joyeer_native_regression(
            E2ERegressionEnumInstances regression_backend_enum_instances
            "42\nhello\n7\n9\n")
    add_joyeer_native_regression(
            E2ERegressionPayloadCoverage regression_type_payload_coverage
            "0\n0\n1\n2\n3\n3\n4\n7\n10\n11\n12\n20\n21\n22\n23\n24\n")
    add_joyeer_native_regression(
            E2ERegressionEvaluationAccesses regression_type_evaluation_accesses
            "2\n7\n4\nupdated\n7\nchanged\n7\n10\n5\n")
    add_joyeer_native_regression(
            E2ERegressionFlowReinitialization regression_flow_reinitialization
            "ready\nready\nold\nnew\niteration\niteration\n")
    set(methods_output "1\n7\n14\ntrue\n0\n15\n1000\n2\n8\n")
    add_joyeer_native_regression(E2EMethods methods "${methods_output}")
    add_joyeer_native_regression(
            E2EMethodsOwnership methods_ownership
            "abcd\nabcdef\n3\n2\n6\nabcdef\n5\n9\nxyz\nxyxyzz\n")
    set_tests_properties(
            E2EMethodsO0 E2EMethodsO2
            E2EMethodsOwnershipO0 E2EMethodsOwnershipO2
            PROPERTIES LABELS "e2e;llvm-backend;runtime;ownership;safety;regression;methods")
    add_test(NAME E2EMethodsFullDebug
            COMMAND ${CMAKE_COMMAND}
                    -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                    -DINPUT_FILE=${JOYEER_TESTS_DIR}/e2e/methods.joyeer
                    -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/methods-debug${CMAKE_EXECUTABLE_SUFFIX}
                    "-DCOMPILER_ARGS=-O0;-gfull"
                    "-DEXPECTED_OUTPUT=${methods_output}"
                    -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake
    )
    set_tests_properties(E2EMethodsFullDebug PROPERTIES
            LABELS "e2e;llvm-backend;runtime;ownership;debug-info;methods"
            TIMEOUT 60)

    set(propagation_output
            "local!\nready!\nok\n1\nlocal!\nfailed!\n2\nfailed!\n3\nfirst!\nready!\n4\nheld!\npresent!\nheld!\nmissing\nunit some\nunit none\n7\nunit!\nscope!\n9\narray ok\narray!\nouter\ninner\n42\nouter\ninner\n42\nkept!\nkept!\nborrowed error!\nborrowed error!\n3\nnever success\n9\nnever none\nfalse\n0\ntrue\n1\nguard failed\n2\n")
    add_joyeer_native_regression(
            E2EPropagation propagation "${propagation_output}")
    set_tests_properties(E2EPropagationO0 E2EPropagationO2 PROPERTIES
            LABELS "e2e;llvm-backend;runtime;ownership;safety;regression;propagation")

    add_test(NAME E2EPropagationFullDebug
            COMMAND ${CMAKE_COMMAND}
                    -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                    -DINPUT_FILE=${JOYEER_TESTS_DIR}/e2e/propagation.joyeer
                    -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/propagation-debug${CMAKE_EXECUTABLE_SUFFIX}
                    "-DCOMPILER_ARGS=-O0;-gfull"
                    "-DEXPECTED_OUTPUT=${propagation_output}"
                    -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake
    )
    set_tests_properties(E2EPropagationFullDebug PROPERTIES
            LABELS "e2e;llvm-backend;runtime;ownership;propagation;debug-info")

    add_test(NAME E2EPropagationFile
            COMMAND ${CMAKE_COMMAND}
                    -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                    -DINPUT_FILE=${JOYEER_TESTS_DIR}/e2e/propagation_file.joyeer
                    -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/propagation-file${CMAKE_EXECUTABLE_SUFFIX}
                    "-DEXPECTED_OUTPUT=file contents\nmissing\n"
                    -DPROGRAM_WORKING_DIRECTORY=${JOYEER_TESTS_DIR}/e2e
                    -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake
    )
    set_tests_properties(E2EPropagationFile PROPERTIES
            LABELS "e2e;llvm-backend;runtime;ownership;file-io;propagation")

    set(unit_values_output
            "true\ntrue\n2\ntrue\nbranch!\ntrue\n3\ntemporary!\n4\ntemporary!\n3\ntrue\n5\n2\ntrue\n6\ntrue\nowned!\nevent\nsome\nnone\nunit error\n100\n")
    add_joyeer_native_regression(
            E2EUnitValues unit_values "${unit_values_output}")
    add_joyeer_native_regression(
            E2EDictionaryGet dictionary_get "dictionary get ok\n" 0 1 2 3)
    add_joyeer_native_regression(
            E2EControlFlowArithmetic control_flow_arithmetic "control flow arithmetic ok\n" 0 1 2 3)
    add_test(NAME E2EControlFlowArithmeticFullDebug
            COMMAND ${CMAKE_COMMAND}
                    -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                    -DINPUT_FILE=${JOYEER_TESTS_DIR}/e2e/control_flow_arithmetic.joyeer
                    -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/control-flow-arithmetic-debug${CMAKE_EXECUTABLE_SUFFIX}
                    "-DCOMPILER_ARGS=-O0;-gfull"
                    "-DEXPECTED_OUTPUT=control flow arithmetic ok\n"
                    -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake
    )
    set_tests_properties(E2EControlFlowArithmeticFullDebug PROPERTIES
            LABELS "e2e;llvm-backend;runtime;ownership;debug-info"
            TIMEOUT 60)
    add_test(NAME E2EDictionaryGetFullDebug
            COMMAND ${CMAKE_COMMAND}
                    -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                    -DINPUT_FILE=${JOYEER_TESTS_DIR}/e2e/dictionary_get.joyeer
                    -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/dictionary-get-debug${CMAKE_EXECUTABLE_SUFFIX}
                    "-DCOMPILER_ARGS=-O0;-gfull"
                    "-DEXPECTED_OUTPUT=dictionary get ok\n"
                    -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake
    )
    set_tests_properties(E2EDictionaryGetFullDebug PROPERTIES
            LABELS "e2e;llvm-backend;runtime;ownership;debug-info"
            TIMEOUT 60)
    add_test(NAME E2EUnitValuesFullDebug
            COMMAND ${CMAKE_COMMAND}
                    -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                    -DINPUT_FILE=${JOYEER_TESTS_DIR}/e2e/unit_values.joyeer
                    -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/unit-values-debug${CMAKE_EXECUTABLE_SUFFIX}
                    "-DCOMPILER_ARGS=-O0;-gfull"
                    "-DEXPECTED_OUTPUT=${unit_values_output}"
                    -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake
    )
    set_tests_properties(E2EUnitValuesFullDebug PROPERTIES
            LABELS "e2e;llvm-backend;ownership;debug-info;unit"
            TIMEOUT 60)

    add_joyeer_native_regression(
            E2ECheckedArithmetic checked_arithmetic
            "9223372036854775807\n-9223372036854775808\n-9223372036854775808\n9223372036854775807\n-9223372036854775808\n0\n21\n2\n49995000\n"
            0 1 2 3)
    add_joyeer_native_regression(
            E2EBitwiseOperators bitwise_operators
            "15728880
4293984240
4278255360
252645135
64
2
126
-4
-64
8
9223372036854775808
252645135
true
6
-1
"
            0 1 2 3)
    add_joyeer_native_regression(
            E2EIntegerConversions integer_conversions
            "300
200
-5
251
4294967291
69
9029
74565
-1
18446744073709551615
43981
255
"
            0 1 2 3)
    add_joyeer_native_regression(
            E2EConstantTables constant_tables
            "3421780262
256
755167117
17
6
beta
6
hello, constants
true
100000
99
2
42
3
114
"
            0 1 2 3)
    add_joyeer_native_regression(
            E2ECompoundAssignment compound_assignment
            "3
680
abcdabcd
101
1
42
255
9
3
1099511627776
"
            0 1 2 3)
    add_joyeer_native_regression(
            E2EAssignmentOrder assignment_order
            "2\n102\n11\n202\na!\n52\n6\n65\n30\n60\n7\n"
            0 1 2 3)
    add_joyeer_native_regression(
            E2EForIn for_in
            "10\na\nbc\nx\ny\n17\n0\n1\n2\n10\n20\n30\n-2\n-1\n255\n0\n1\n2\n16\ntick\ntick\n104\n105\n10\n20\n30\n6\n0\n1\n10\n11\np\nq\npq\n"
            0 1 2 3)
    add_joyeer_native_regression(
            E2ETryConversions conversion_try
            "true\ntrue\nfalse\nfalse\ntrue\ntrue\nfalse\nfalse\ntrue\ntrue\nfalse\nfalse\ntrue\ntrue\nfalse\nfalse\ntrue\ntrue\nfalse\nfalse\ntrue\ntrue\nfalse\nfalse\ntrue\ntrue\nfalse\ntrue\ntrue\nfalse\nfalse\nfalse\ntrue\ntrue\ntrue\ntrue\nfalse\nfalse\ntrue\nfalse\nfalse\ntrue\ntrue\ntrue\nfalse\n200\n-1\n-3\n"
            0 1 2 3)
    add_joyeer_native_regression(
            E2ERadixLiterals radix_literals
            "255
3988292384
165
493
18446744073709551615
1000000
-16
1000.5
"
            0 1 2 3)
    set(typed_array_output
            "2\n40\n50\n30\n122\nfalse\ntrue\ntrue\nsecond\nchanged\n2\n9\n121\n9\ntrue\n11\nowned\n2\ntrue\n")
    add_joyeer_native_regression(
            E2ETypedArrayAccess typed_array_access "${typed_array_output}"
            0 1 2 3)
    add_test(NAME E2ECheckedAccessDebugInfo
            COMMAND ${CMAKE_COMMAND}
                    -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                    -DINPUT_FILE=${JOYEER_TESTS_DIR}/e2e/typed_array_access.joyeer
                    -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/joyeer-checked-debug${CMAKE_EXECUTABLE_SUFFIX}
                    "-DCOMPILER_ARGS=-O3;-gfull"
                    "-DEXPECTED_OUTPUT=${typed_array_output}"
                    -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyNativeExecutable.cmake
    )
    set_tests_properties(E2ECheckedAccessDebugInfo PROPERTIES
            LABELS "e2e;llvm-backend;optimization;safety;debug-info")

    function(add_joyeer_native_checked_trap name fixture diagnostic)
        foreach(level IN ITEMS 0 1 2 3)
            add_test(NAME ${name}O${level}
                    COMMAND ${CMAKE_COMMAND}
                            -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                            -DINPUT_FILE=${JOYEER_TESTS_DIR}/e2e/${fixture}.joyeer
                            -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/${fixture}-trap-O${level}${CMAKE_EXECUTABLE_SUFFIX}
                            -DCOMPILER_ARGS=-O${level}
                            "-DEXPECTED_PATTERN=${diagnostic}"
                            -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyOptimizedNativeFailure.cmake
            )
            set_tests_properties(${name}O${level} PROPERTIES
                    LABELS "e2e;llvm-backend;runtime;optimization;safety"
                    TIMEOUT 60)
        endforeach()
    endfunction()
    add_joyeer_native_checked_trap(
            E2EUnitArrayBounds unit_array_bounds "array index out of bounds")
    add_joyeer_native_checked_trap(
            E2EUnitDictionaryMissing unit_dictionary_missing "dictionary key not found")

    # Runtime-loaded operands keep these checks from folding to unconditional
    # panic calls; E2EOptimizedOverflowTrap still covers constant overflow.
    add_joyeer_native_checked_trap(
            E2ECheckedAddOverflow checked_add_overflow "integer addition overflow")
    add_joyeer_native_checked_trap(
            E2ECheckedSubtractOverflow checked_subtract_overflow "integer subtraction overflow")
    add_joyeer_native_checked_trap(
            E2ECheckedMultiplyOverflow checked_multiply_overflow "integer multiplication overflow")
    add_joyeer_native_checked_trap(
            E2ECheckedMinMultiplyOverflow checked_min_multiply_overflow "integer multiplication overflow")
    add_joyeer_native_checked_trap(
            E2ECheckedDivideZero checked_divide_zero "integer division by zero")
    add_joyeer_native_checked_trap(
            E2EBitwiseShiftOverflow bitwise_shift_overflow "integer shift count out of range")
    add_joyeer_native_checked_trap(
            E2EBitwiseShiftNegative bitwise_shift_negative "integer shift count out of range")
    add_joyeer_native_checked_trap(
            E2EConversionNegative conversion_negative "integer conversion out of range")
    add_joyeer_native_checked_trap(
            E2EConversionNarrow conversion_narrow "integer conversion out of range")
    add_joyeer_native_checked_trap(
            E2EConversionUnsignedHigh conversion_unsigned_high "integer conversion out of range")
    add_joyeer_native_checked_trap(
            E2ECompoundOverflow compound_overflow "integer addition overflow")
    add_joyeer_native_checked_trap(
            E2ECompoundShiftRange compound_shift_range "integer shift count out of range")
    add_joyeer_native_checked_trap(
            E2ECheckedRemainderZero checked_remainder_zero "integer remainder by zero")
    add_joyeer_native_checked_trap(
            E2ECheckedDivideOverflow checked_divide_overflow "integer division overflow")
    add_joyeer_native_checked_trap(
            E2ECheckedRemainderOverflow checked_remainder_overflow "integer remainder overflow")
    add_joyeer_native_checked_trap(
            E2ECheckedArrayUpper optimized_bounds "array index out of bounds")
    add_joyeer_native_checked_trap(
            E2ECheckedArrayNegative checked_array_negative_index "array index out of bounds")
    add_joyeer_native_checked_trap(
            E2ECheckedArrayEmpty checked_array_empty_index "array index out of bounds")
    add_joyeer_native_checked_trap(
            E2ECheckedArrayWrite checked_array_write_index "array index out of bounds")
    add_joyeer_native_checked_trap(
            E2ECheckedArrayMax checked_array_max_index "array index out of bounds")
    add_joyeer_native_checked_trap(
            E2ECheckedStringNegative checked_string_negative_index "string index out of bounds")
    add_joyeer_native_checked_trap(
            E2ECheckedStringPastEnd checked_string_index_past_end "string index out of bounds")
    add_joyeer_native_checked_trap(
            E2ECheckedStringEmpty checked_string_empty_index "string index out of bounds")
    add_joyeer_native_checked_trap(
            E2ECheckedIndexOverflow checked_index_overflow "integer addition overflow")
    add_joyeer_native_checked_trap(
            E2ECheckedBoundsBeforeOverflow checked_bounds_before_overflow "array index out of bounds")

    add_test(NAME E2EOptimizedOverflowTrap
            COMMAND ${CMAKE_COMMAND}
                    -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                    -DINPUT_FILE=${JOYEER_TESTS_DIR}/e2e/optimized_overflow.joyeer
                    -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/joyeer-native-optimized-overflow${CMAKE_EXECUTABLE_SUFFIX}
                    "-DEXPECTED_PATTERN=integer addition overflow"
                    -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyOptimizedNativeFailure.cmake
    )
    set_tests_properties(E2EOptimizedOverflowTrap PROPERTIES
            LABELS "e2e;llvm-backend;runtime;optimization;safety"
    )

    add_test(NAME E2EOptimizedBoundsTrap
            COMMAND ${CMAKE_COMMAND}
                    -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                    -DINPUT_FILE=${JOYEER_TESTS_DIR}/e2e/optimized_bounds.joyeer
                    -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/joyeer-native-optimized-bounds${CMAKE_EXECUTABLE_SUFFIX}
                    "-DEXPECTED_PATTERN=array index out of bounds"
                    -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyOptimizedNativeFailure.cmake
    )
    set_tests_properties(E2EOptimizedBoundsTrap PROPERTIES
            LABELS "e2e;llvm-backend;runtime;optimization;safety"
    )

    # Differential check: every self-contained native fixture must behave the
    # same (exit status, stdout, stderr) at -O0 through -O3. Fixtures that need
    # program arguments, a working directory, input files, or that are meant to
    # fail compilation are excluded.
    file(GLOB joyeer_native_fixtures "${JOYEER_TESTS_DIR}/e2e/*.joyeer")
    set(joyeer_equivalence_excluded
            entry_argument_bytes entry_arguments entry_exit_status
            filesystem file-kind-device process read_file propagation_file
            json_parser missing_main full_debug)
    foreach(fixture_path IN LISTS joyeer_native_fixtures)
        get_filename_component(fixture "${fixture_path}" NAME_WE)
        if(fixture IN_LIST joyeer_equivalence_excluded)
            continue()
        endif()
        add_test(NAME E2EOptimizationEquivalence_${fixture}
                COMMAND ${CMAKE_COMMAND}
                        -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                        -DINPUT_FILE=${fixture_path}
                        -DOUTPUT_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/equivalence-${fixture}
                        -DCMAKE_EXECUTABLE_SUFFIX=${CMAKE_EXECUTABLE_SUFFIX}
                        -P ${JOYEER_SOURCE_DIR}/scripts/cmake/verifyOptimizationEquivalence.cmake
        )
        set_tests_properties(E2EOptimizationEquivalence_${fixture} PROPERTIES
                LABELS "e2e;llvm-backend;optimization;differential"
                TIMEOUT 240)
    endforeach()
endif()

function(add_joyeer_rejected_regression name stage fixture diagnostic)
    add_test(NAME ${name}
            COMMAND ${CMAKE_COMMAND}
                    -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                    -DINPUT_FILE=${JOYEER_TESTS_DIR}/${stage}/err/${fixture}.joyeer
                    -DEXPECTED_PATTERN=${diagnostic}
                    -P ${JOYEER_SOURCE_DIR}/scripts/cmake/expectCompilerFailure.cmake
    )
    set_tests_properties(${name} PROPERTIES LABELS "${stage};safety;regression" TIMEOUT 30)
endfunction()

add_joyeer_rejected_regression(TypeRegressionPayloadCoverage type-checking
        regression_type_payload_nonexhaustive type-checking.non-exhaustive-match)
add_joyeer_rejected_regression(TypeDictionaryGetWrongKey type-checking
        dictionary_get_key type-checking.type-mismatch)
add_joyeer_rejected_regression(TypeDictionaryGetNoFlatten type-checking
        dictionary_get_flatten type-checking.type-mismatch)
add_joyeer_rejected_regression(TypeDictionaryGetExclusivity type-checking
        dictionary_get_exclusivity type-checking.overlapping-access)
add_joyeer_rejected_regression(TypeDictionaryGetArguments type-checking
        dictionary_get_arguments type-checking.invalid-call-arguments)
add_joyeer_rejected_regression(TypeLoopControlOutside type-checking
        loop_control_outside type-checking.invalid-loop-control)
add_joyeer_rejected_regression(TypeForInNotIterable type-checking
        for_in_not_iterable type-checking.not-iterable)
add_joyeer_rejected_regression(TypeForInDictionary type-checking
        for_in_dictionary type-checking.not-iterable)
add_joyeer_rejected_regression(TypeControlFlowOperands type-checking
        control_flow_operands type-checking.invalid-operator-operands)
add_joyeer_rejected_regression(FlowLoopContinueConsumed semantic-analysis
        loop_continue_consumed semantic-analysis.use-after-consume)
add_joyeer_rejected_regression(FlowLoopBreakConsumed semantic-analysis
        loop_break_consumed semantic-analysis.use-after-consume)
add_joyeer_rejected_regression(FlowLogicalOrInitialization semantic-analysis
        logical_or_initialization semantic-analysis.use-before-initialization)
add_joyeer_rejected_regression(TypePropagationWrongReturn type-checking
        propagation_wrong_return type-checking.invalid-propagation-context)
add_joyeer_rejected_regression(TypePropagationWrongError type-checking
        propagation_wrong_error type-checking.mismatched-propagation-error)
set_tests_properties(TypePropagationWrongReturn TypePropagationWrongError PROPERTIES
        LABELS "type-checking;safety;regression;propagation")
add_joyeer_rejected_regression(TypeRegressionConditionalExclusivity type-checking
        regression_type_conditional_exclusivity type-checking.overlapping-access)
add_joyeer_rejected_regression(TypeRegressionReceiverExclusivity type-checking
        regression_type_receiver_exclusivity type-checking.overlapping-access)
add_joyeer_rejected_regression(FlowRegressionInitializingEarlyReturn semantic-analysis
        regression_flow_initializing_early_return semantic-analysis.initializing-parameter-not-initialized)
add_joyeer_rejected_regression(FlowRegressionWrongElement semantic-analysis
        regression_flow_wrong_element semantic-analysis.use-after-consume)
add_joyeer_rejected_regression(FlowRegressionLoopCondition semantic-analysis
        regression_flow_loop_condition semantic-analysis.use-after-consume)
add_joyeer_rejected_regression(FlowRegressionProjectionIndex semantic-analysis
        regression_flow_projection_index semantic-analysis.use-before-initialization)
add_joyeer_rejected_regression(FlowRegressionPossibleInitialization semantic-analysis
        regression_flow_possible_initialization semantic-analysis.initializing-initialized-storage)

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

add_test(NAME TypeCheckingCliRejectsUnitPayload
        COMMAND ${CMAKE_COMMAND}
                -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                -DINPUT_FILE=${JOYEER_TESTS_DIR}/type-checking/err/unit_result_payload.joyeer
                -DEXPECTED_PATTERN=type-checking.type-mismatch
                -P ${JOYEER_SOURCE_DIR}/scripts/cmake/expectCompilerFailure.cmake)
set_tests_properties(TypeCheckingCliRejectsUnitPayload PROPERTIES LABELS "type-checking;unit")

add_test(NAME SemanticAnalysisCliRejectsUninitializedUnit
        COMMAND ${CMAKE_COMMAND}
                -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                -DINPUT_FILE=${JOYEER_TESTS_DIR}/semantic-analysis/err/unit_uninitialized.joyeer
                -DEXPECTED_PATTERN=semantic-analysis.use-before-initialization
                -P ${JOYEER_SOURCE_DIR}/scripts/cmake/expectCompilerFailure.cmake)
set_tests_properties(SemanticAnalysisCliRejectsUninitializedUnit PROPERTIES LABELS "semantic-analysis;unit")

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

add_test(NAME E2ELinkerRejectsMissingMain
        COMMAND ${CMAKE_COMMAND}
                -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                -DINPUT_FILE=${JOYEER_TESTS_DIR}/e2e/missing_main.joyeer
                -DEXPECTED_PATTERN=linker.missing-entry-point
                "-DEXTRA_ARGS=-o;${CMAKE_CURRENT_BINARY_DIR}/missing-main${CMAKE_EXECUTABLE_SUFFIX}"
                -DOUTPUT_TO_REMOVE=${CMAKE_CURRENT_BINARY_DIR}/missing-main${CMAKE_EXECUTABLE_SUFFIX}
                -P ${JOYEER_SOURCE_DIR}/scripts/cmake/expectCompilerFailure.cmake
)
set_tests_properties(E2ELinkerRejectsMissingMain PROPERTIES
        LABELS "e2e;llvm-backend"
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

add_test(NAME DiagnosticsCliRejectsCopyOfNoncopyableValue
        COMMAND ${CMAKE_COMMAND}
                -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                -DINPUT_FILE=${JOYEER_TESTS_DIR}/type-checking/err/deinit_copy.joyeer
                "-DEXPECTED_HEADER=deinit_copy.joyeer:8:13: error[type-checking.copy-of-noncopyable]"
                "-DEXPECTED_SOURCE=    let b = a"
                "-DEXPECTED_HELP=help: move an owning local or consuming parameter with 'consume'"
                -P ${JOYEER_SOURCE_DIR}/scripts/cmake/expectRichCompilerDiagnostic.cmake
)
set_tests_properties(DiagnosticsCliRejectsCopyOfNoncopyableValue PROPERTIES
        LABELS "diagnostics;type-checking;deinit"
)

add_test(NAME DiagnosticsCliRejectsPartialConsumeOfDeinitType
        COMMAND ${CMAKE_COMMAND}
                -DJOYEER_EXECUTABLE=$<TARGET_FILE:joyeer>
                -DINPUT_FILE=${JOYEER_TESTS_DIR}/semantic-analysis/err/deinit_partial_consume.joyeer
                "-DEXPECTED_HEADER=deinit_partial_consume.joyeer:10:25: error[semantic-analysis.partial-consume-of-deinit-type]"
                "-DEXPECTED_SOURCE=    take(value: consume handle.name)"
                -P ${JOYEER_SOURCE_DIR}/scripts/cmake/expectRichCompilerDiagnostic.cmake
)
set_tests_properties(DiagnosticsCliRejectsPartialConsumeOfDeinitType PROPERTIES
        LABELS "diagnostics;semantic-analysis;deinit"
)
