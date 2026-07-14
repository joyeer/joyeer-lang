#include "joyeer/backend/llvm.h"

#include <fstream>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc != 2) return 2;

    joyeer::ir::Module module;
    module.sourceName = "clang-validation.joyeer";
    module.types = {
        joyeer::ir::TypeName {
            0,
            "Void",
            joyeer::typing::TypeKind::voidType,
        },
        joyeer::ir::TypeName {
            1,
            "Int",
            joyeer::typing::TypeKind::integer,
        },
    };

    joyeer::ir::Function function;
    function.id = 0;
    function.name = "add";
    function.parameters = {
        joyeer::ir::Parameter {
            joyeer::ir::Value { 0, 1, joyeer::ir::ValueCategory::value },
            std::nullopt,
            "left",
        },
        joyeer::ir::Parameter {
            joyeer::ir::Value { 1, 1, joyeer::ir::ValueCategory::value },
            std::nullopt,
            "right",
        },
    };
    function.resultType = 1;
    function.returnsValue = true;
    function.entry = 0;
    function.blocks = {
        joyeer::ir::BasicBlock {
            0,
            "entry",
            {
                joyeer::ir::Instruction {
                    joyeer::ir::Opcode::add,
                    joyeer::ir::Value { 2, 1, joyeer::ir::ValueCategory::value },
                    { 0, 1 },
                },
                joyeer::ir::Instruction {
                    joyeer::ir::Opcode::returnValue,
                    std::nullopt,
                    { 2 },
                },
            },
        },
    };
    module.functions.push_back(std::move(function));

    const auto result = joyeer::llvmbackend::Emitter().emit(module);
    if (!result.succeeded()) {
        std::cerr << joyeer::llvmbackend::dump(result.diagnostics);
        return 3;
    }
    std::ofstream output(argv[1], std::ios::binary);
    if (!output) return 4;
    output << result.text;
    return output.good() ? 0 : 5;
}
