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
        joyeer::ir::TypeName {
            2,
            "Pair",
            joyeer::typing::TypeKind::structure,
            100,
        },
        joyeer::ir::TypeName {
            3,
            "Choice",
            joyeer::typing::TypeKind::enumeration,
            200,
        },
    };

    module.structures.push_back(joyeer::ir::StructureDefinition {
        100,
        2,
        "Pair",
        {
            joyeer::ir::FieldDefinition { 101, "left", 1, false },
            joyeer::ir::FieldDefinition { 102, "right", 1, false },
        },
    });
    module.enumerations.push_back(joyeer::ir::EnumerationDefinition {
        200,
        3,
        "Choice",
        {
            joyeer::ir::EnumCaseDefinition { 201, "None", {} },
            joyeer::ir::EnumCaseDefinition { 202, "Some", { 1 } },
        },
    });

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

    joyeer::ir::Function makePair;
    makePair.id = 1;
    makePair.name = "makePair";
    makePair.resultType = 1;
    makePair.returnsValue = true;
    makePair.entry = 0;
    makePair.blocks = {
        joyeer::ir::BasicBlock {
            0,
            "entry",
            {
                joyeer::ir::Instruction {
                    joyeer::ir::Opcode::integerConstant,
                    joyeer::ir::Value { 0, 1, joyeer::ir::ValueCategory::value },
                    {},
                    {},
                    std::nullopt,
                    std::nullopt,
                    1,
                },
                joyeer::ir::Instruction {
                    joyeer::ir::Opcode::integerConstant,
                    joyeer::ir::Value { 1, 1, joyeer::ir::ValueCategory::value },
                    {},
                    {},
                    std::nullopt,
                    std::nullopt,
                    2,
                },
                joyeer::ir::Instruction {
                    joyeer::ir::Opcode::constructStruct,
                    joyeer::ir::Value { 2, 2, joyeer::ir::ValueCategory::value },
                    { 0, 1 },
                    {},
                    std::nullopt,
                    100,
                },
                joyeer::ir::Instruction {
                    joyeer::ir::Opcode::extractField,
                    joyeer::ir::Value { 3, 1, joyeer::ir::ValueCategory::value },
                    { 2 },
                    {},
                    std::nullopt,
                    102,
                },
                joyeer::ir::Instruction {
                    joyeer::ir::Opcode::returnValue,
                    std::nullopt,
                    { 3 },
                },
            },
        },
    };
    module.functions.push_back(std::move(makePair));

    joyeer::ir::Function readChoice;
    readChoice.id = 2;
    readChoice.name = "readChoice";
    readChoice.parameters = {
        joyeer::ir::Parameter {
            joyeer::ir::Value { 0, 3, joyeer::ir::ValueCategory::value },
            std::nullopt,
            "choice",
        },
    };
    readChoice.resultType = 1;
    readChoice.returnsValue = true;
    readChoice.entry = 0;
    readChoice.blocks = {
        joyeer::ir::BasicBlock {
            0,
            "entry",
            {
                joyeer::ir::Instruction {
                    joyeer::ir::Opcode::switchPattern,
                    std::nullopt,
                    { 0 },
                    {},
                    std::nullopt,
                    std::nullopt,
                    0,
                    {},
                    {},
                    {
                        joyeer::ir::SwitchCase {
                            joyeer::ir::Pattern {
                                joyeer::ir::PatternKind::enumCase,
                                3,
                                201,
                            },
                            1,
                        },
                        joyeer::ir::SwitchCase {
                            joyeer::ir::Pattern {
                                joyeer::ir::PatternKind::enumCase,
                                3,
                                202,
                                0,
                                {},
                                {
                                    joyeer::ir::Pattern {
                                        joyeer::ir::PatternKind::wildcard,
                                        1,
                                    },
                                },
                            },
                            2,
                        },
                    },
                },
            },
        },
        joyeer::ir::BasicBlock {
            1,
            "none",
            {
                joyeer::ir::Instruction {
                    joyeer::ir::Opcode::integerConstant,
                    joyeer::ir::Value { 1, 1, joyeer::ir::ValueCategory::value },
                },
                joyeer::ir::Instruction {
                    joyeer::ir::Opcode::returnValue,
                    std::nullopt,
                    { 1 },
                },
            },
        },
        joyeer::ir::BasicBlock {
            2,
            "some",
            {
                joyeer::ir::Instruction {
                    joyeer::ir::Opcode::extractPayload,
                    joyeer::ir::Value { 2, 1, joyeer::ir::ValueCategory::value },
                    { 0 },
                    {},
                    std::nullopt,
                    202,
                },
                joyeer::ir::Instruction {
                    joyeer::ir::Opcode::returnValue,
                    std::nullopt,
                    { 2 },
                },
            },
        },
    };
    module.functions.push_back(std::move(readChoice));

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
