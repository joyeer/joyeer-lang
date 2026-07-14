
add_joyeer_library(JoyeerCompiler STATIC
        compiler+service.cpp
        context.cpp
        lexparser.cpp
        IRGen.cpp
        irlowering.cpp
        node.cpp
        node+visitor.cpp
        nameresolution.cpp
        parser.cpp
        semanticanalysis.cpp
        semantic.cpp
        debugprinter.cpp
        sourcefile.cpp
        symtable.cpp
        syntax.cpp
        syntaxparser.cpp
        token.cpp
        typechecking.cpp
        typegen.cpp
        typebinding.cpp
        )

target_link_libraries(JoyeerCompiler PUBLIC JoyeerIR)
target_link_libraries(JoyeerCompiler PUBLIC JoyeerLLVMBackend)

