
add_joyeer_library(JoyeerCompiler STATIC
        compiler+service.cpp
        irlowering.cpp
        lexparser.cpp
        nameresolution.cpp
        parser.cpp
        semanticanalysis.cpp
        semantic.cpp
        sourcefile.cpp
        syntax.cpp
        token.cpp
        typechecking.cpp
        )

target_link_libraries(JoyeerCompiler PUBLIC JoyeerIR)
target_link_libraries(JoyeerCompiler PUBLIC JoyeerLLVMBackend)
target_link_libraries(JoyeerCompiler PUBLIC JoyeerDiagnostic)

