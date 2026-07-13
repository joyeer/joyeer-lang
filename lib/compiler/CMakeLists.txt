
add_joyeer_library(JoyeerCompiler STATIC
        compiler+service.cpp
        context.cpp
        lexparser.cpp
        IRGen.cpp
        node.cpp
        node+visitor.cpp
        parser.cpp
        debugprinter.cpp
        sourcefile.cpp
        symtable.cpp
        syntax.cpp
        syntaxparser.cpp
        token.cpp
        typegen.cpp
        typebinding.cpp
        )

