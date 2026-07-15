#include "joyeer/backend/llvm.h"

#include <fstream>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) return 2;
    const auto debugOption = argc == 3 ? std::string(argv[2]) : std::string();
    const bool emitDebugInfo = debugOption == "--debug-dwarf" ||
            debugOption == "--debug-codeview";
    if (argc == 3 && !emitDebugInfo) return 2;

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
        joyeer::ir::TypeName {
            4,
            "String",
            joyeer::typing::TypeKind::string,
        },
        joyeer::ir::TypeName {
            5,
            "[Int]",
            joyeer::typing::TypeKind::array,
            joyeer::semantic::invalidSymbolId,
            { 1 },
        },
        joyeer::ir::TypeName {
            6,
            "[String: Int]",
            joyeer::typing::TypeKind::dictionary,
            joyeer::semantic::invalidSymbolId,
            { 4, 1 },
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

    joyeer::ir::Function collections;
    collections.id = 3;
    collections.name = "collections";
    collections.resultType = 1;
    collections.returnsValue = true;
    collections.entry = 0;
    collections.blocks = {
        joyeer::ir::BasicBlock {
            0,
            "entry",
            {
                joyeer::ir::Instruction {
                    joyeer::ir::Opcode::integerConstant,
                    joyeer::ir::Value { 0, 1, joyeer::ir::ValueCategory::value },
                    {}, {}, std::nullopt, std::nullopt, 0,
                },
                joyeer::ir::Instruction {
                    joyeer::ir::Opcode::integerConstant,
                    joyeer::ir::Value { 1, 1, joyeer::ir::ValueCategory::value },
                    {}, {}, std::nullopt, std::nullopt, 2,
                },
                joyeer::ir::Instruction {
                    joyeer::ir::Opcode::constructArray,
                    joyeer::ir::Value { 2, 5, joyeer::ir::ValueCategory::value },
                    { 0, 1 },
                },
                joyeer::ir::Instruction {
                    joyeer::ir::Opcode::subscript,
                    joyeer::ir::Value { 3, 1, joyeer::ir::ValueCategory::value },
                    { 2, 0 },
                },
                joyeer::ir::Instruction {
                    joyeer::ir::Opcode::stringConstant,
                    joyeer::ir::Value { 4, 4, joyeer::ir::ValueCategory::value },
                    {}, {}, std::nullopt, std::nullopt, 0, "key",
                },
                joyeer::ir::Instruction {
                    joyeer::ir::Opcode::integerConstant,
                    joyeer::ir::Value { 5, 1, joyeer::ir::ValueCategory::value },
                    {}, {}, std::nullopt, std::nullopt, 42,
                },
                joyeer::ir::Instruction {
                    joyeer::ir::Opcode::constructDictionary,
                    joyeer::ir::Value { 6, 6, joyeer::ir::ValueCategory::value },
                    { 4, 5 },
                },
                joyeer::ir::Instruction {
                    joyeer::ir::Opcode::subscript,
                    joyeer::ir::Value { 7, 1, joyeer::ir::ValueCategory::value },
                    { 6, 4 },
                },
                joyeer::ir::Instruction {
                    joyeer::ir::Opcode::add,
                    joyeer::ir::Value { 8, 1, joyeer::ir::ValueCategory::value },
                    { 3, 7 },
                },
                joyeer::ir::Instruction {
                    joyeer::ir::Opcode::returnValue,
                    std::nullopt,
                    { 8 },
                },
            },
        },
    };
    module.functions.push_back(std::move(collections));

    joyeer::ir::Function ownership;
    ownership.id = 4;
    ownership.name = "ownership";
    ownership.resultType = 0;
    ownership.entry = 0;
    ownership.blocks = {
        joyeer::ir::BasicBlock {
            0,
            "entry",
            {
                joyeer::ir::Instruction {
                    joyeer::ir::Opcode::stringConstant,
                    joyeer::ir::Value { 0, 4, joyeer::ir::ValueCategory::value },
                    {}, {}, std::nullopt, std::nullopt, 0, "owned",
                },
                joyeer::ir::Instruction {
                    joyeer::ir::Opcode::copyValue,
                    joyeer::ir::Value { 1, 4, joyeer::ir::ValueCategory::value },
                    { 0 },
                },
                joyeer::ir::Instruction {
                    joyeer::ir::Opcode::stackAllocate,
                    joyeer::ir::Value { 2, 4, joyeer::ir::ValueCategory::address },
                },
                joyeer::ir::Instruction {
                    joyeer::ir::Opcode::store,
                    std::nullopt,
                    { 1, 2 },
                },
                joyeer::ir::Instruction {
                    joyeer::ir::Opcode::take,
                    joyeer::ir::Value { 3, 4, joyeer::ir::ValueCategory::value },
                    { 2 },
                },
                joyeer::ir::Instruction {
                    joyeer::ir::Opcode::stackAllocate,
                    joyeer::ir::Value { 4, 4, joyeer::ir::ValueCategory::address },
                },
                joyeer::ir::Instruction {
                    joyeer::ir::Opcode::store,
                    std::nullopt,
                    { 3, 4 },
                },
                joyeer::ir::Instruction {
                    joyeer::ir::Opcode::destroy,
                    std::nullopt,
                    { 4 },
                },
                joyeer::ir::Instruction { joyeer::ir::Opcode::returnVoid },
            },
        },
    };
    module.functions.push_back(std::move(ownership));

    if (emitDebugInfo) {
        module.sourceInfo = joyeer::ir::SourceInfo {
            "clang-validation.joyeer",
            "C:/joyeer-tests",
            64,
            { 0, 16, 32, 48 },
        };
        for (auto& sourceFunction : module.functions) {
            if (sourceFunction.isExternal) continue;
            sourceFunction.debugLocation = joyeer::ir::DebugLocation {
                SourceSpan { static_cast<uint32_t>(sourceFunction.id * 4), 1 },
                false,
            };
            for (auto& block : sourceFunction.blocks) {
                for (auto& instruction : block.instructions) {
                    instruction.debugLocation = joyeer::ir::DebugLocation {
                        SourceSpan { 16, 1 },
                        instruction.opcode == joyeer::ir::Opcode::destroy,
                    };
                }
            }
        }
    }

    const auto result = joyeer::llvmbackend::Emitter().emit(
            module,
            joyeer::llvmbackend::EmitOptions {
                emitDebugInfo,
                debugOption == "--debug-codeview"
                        ? joyeer::llvmbackend::DebugInfoFormat::codeView
                        : joyeer::llvmbackend::DebugInfoFormat::dwarf,
                joyeer::OptimizationLevel::O0,
            });
    if (!result.succeeded()) {
        std::cerr << joyeer::llvmbackend::dump(result.diagnostics);
        return 3;
    }
    std::ofstream output(argv[1], std::ios::binary);
    if (!output) return 4;
    output << result.text;
    return output.good() ? 0 : 5;
}
