#ifndef JOYEER_BACKEND_IR_WIRE_H
#define JOYEER_BACKEND_IR_WIRE_H

#include "compiler/ir/ir.h"

#include <string_view>
#include <type_traits>

namespace joyeer::llvmbackend::wire {

inline constexpr uint32_t version = 6;

// This private wire format carries Joyeer IR, not LLVM assembly. Both sides
// allocate their own C++ objects; only borrowed bytes cross the backend C ABI.
// Keep the field order synchronized through these shared archive visitors.
template<class Archive> void fields(Archive& archive, SourceSpan& value) {
    archive(value.offset, value.length, value.sourceId);
}
template<class Archive> void fields(Archive& archive, ir::Value& value) {
    archive(value.id, value.type, value.category);
}
template<class Archive> void fields(Archive& archive, ir::Pattern& value) {
    archive(value.kind, value.type, value.symbol, value.integerValue, value.text, value.payloads);
}
template<class Archive> void fields(Archive& archive, ir::SwitchCase& value) {
    archive(value.pattern, value.target);
}
template<class Archive> void fields(Archive& archive, ir::DebugLocation& value) {
    archive(value.span, value.implicitCode, value.scope);
}
template<class Archive> void fields(Archive& archive, ir::SourceInfo& value) {
    archive(value.fileName, value.directory, value.byteLength, value.lineStarts);
}
template<class Archive> void fields(Archive& archive, ir::DebugScope& value) {
    archive(value.id, value.kind, value.function, value.parent, value.semanticScope, value.span);
}
template<class Archive> void fields(Archive& archive, ir::DebugVariable& value) {
    archive(value.id, value.kind, value.function, value.scope, value.symbol, value.name,
            value.type, value.span, value.parameterIndex, value.isMutable);
}
template<class Archive> void fields(Archive& archive, ir::DebugVariableBinding& value) {
    archive(value.variable, value.address);
}
template<class Archive> void fields(Archive& archive, ir::Instruction& value) {
    archive(value.opcode, value.result, value.operands, value.targets, value.callee,
            value.symbol, value.integerValue, value.text, value.span, value.switchCases,
            value.debugLocation, value.debugVariableBindings);
}
template<class Archive> void fields(Archive& archive, ir::BasicBlock& value) {
    archive(value.id, value.name, value.instructions);
}
template<class Archive> void fields(Archive& archive, ir::Parameter& value) {
    archive(value.value, value.symbol, value.name, value.isMutable, value.span,
            value.acceptsAnyType, value.isConsuming, value.isInitializing, value.label);
}
template<class Archive> void fields(Archive& archive, NativeBinding& value) {
    archive(value.kind, value.symbol, value.span);
}
template<class Archive> void fields(Archive& archive, ir::Function& value) {
    archive(value.id, value.symbol, value.name, value.parameters, value.resultType,
            value.returnsValue, value.isExternal, value.entry, value.blocks,
            value.debugLocation, value.debugScope, value.entryDebugVariableBindings,
            value.isRootModule, value.runtimeContext, value.nativeBinding);
}
template<class Archive> void fields(Archive& archive, ir::TypeName& value) {
    archive(value.id, value.name, value.kind, value.symbol, value.arguments, value.declarationSpan);
}
template<class Archive> void fields(Archive& archive, ir::FieldDefinition& value) {
    archive(value.symbol, value.name, value.type, value.isMutable);
}
template<class Archive> void fields(Archive& archive, ir::StructureDefinition& value) {
    archive(value.symbol, value.type, value.name, value.fields);
}
template<class Archive> void fields(Archive& archive, ir::EnumCaseDefinition& value) {
    archive(value.symbol, value.name, value.payloadTypes);
}
template<class Archive> void fields(Archive& archive, ir::EnumerationDefinition& value) {
    archive(value.symbol, value.type, value.name, value.cases);
}
template<class Archive> void fields(Archive& archive, ir::Constant& value) {
    archive(value.id, value.symbol, value.type, value.name, value.data);
}
template<class Archive> void fields(Archive& archive, ir::Module& value) {
    archive(value.sourceName, value.types, value.structures, value.enumerations,
            value.functions, value.constants, value.sourceInfo, value.debugScopes, value.debugVariables,
            value.sourceFiles, value.isRuntimeObject);
}

class Writer {
public:
    std::string bytes;

    template<class... Values> void operator()(const Values&... values) {
        (write(values), ...);
    }

private:
    template<class Value> void write(const Value& value) {
        if constexpr (std::is_integral_v<Value> || std::is_enum_v<Value>) {
            const auto bits = static_cast<uint64_t>(value);
            for (unsigned shift = 0; shift < 64; shift += 8) {
                bytes.push_back(static_cast<char>(bits >> shift));
            }
        } else {
            fields(*this, const_cast<Value&>(value));
        }
    }
    void write(const std::string& value) {
        write(static_cast<uint64_t>(value.size()));
        bytes.append(value);
    }
    template<class Value> void write(const std::vector<Value>& values) {
        write(static_cast<uint64_t>(values.size()));
        for (const auto& value : values) write(value);
    }
    template<class Value> void write(const std::optional<Value>& value) {
        write(value.has_value());
        if (value.has_value()) write(*value);
    }
};

class Reader {
public:
    explicit Reader(std::string_view bytes) : bytes(bytes) {}

    template<class... Values> void operator()(Values&... values) {
        (read(values), ...);
    }
    bool succeeded() const { return valid && bytes.empty(); }

private:
    std::string_view bytes;
    bool valid = true;
    size_t depth = 0;

    template<class Value> void read(Value& value) {
        if (!valid) return;
        if constexpr (std::is_integral_v<Value> || std::is_enum_v<Value>) {
            if (bytes.size() < 8) { valid = false; return; }
            uint64_t bits = 0;
            for (unsigned index = 0; index < 8; ++index) {
                bits |= static_cast<uint64_t>(static_cast<unsigned char>(bytes[index])) << (index * 8);
            }
            bytes.remove_prefix(8);
            value = static_cast<Value>(bits);
        } else {
            // Patterns are recursive; reject malformed input before exhausting
            // the host stack. The frontend verifier remains the semantic gate.
            if (++depth > 256) { valid = false; --depth; return; }
            fields(*this, value);
            --depth;
        }
    }
    void read(std::string& value) {
        uint64_t size = 0;
        read(size);
        if (!valid || size > bytes.size()) { valid = false; return; }
        value.assign(bytes.substr(0, static_cast<size_t>(size)));
        bytes.remove_prefix(static_cast<size_t>(size));
    }
    template<class Value> void read(std::vector<Value>& values) {
        uint64_t size = 0;
        read(size);
        if (!valid || size > bytes.size() / 8) { valid = false; return; }
        values.resize(static_cast<size_t>(size));
        for (auto& value : values) read(value);
    }
    template<class Value> void read(std::optional<Value>& value) {
        bool present = false;
        read(present);
        if (present && valid) {
            value.emplace();
            read(*value);
        } else {
            value.reset();
        }
    }
};

} // namespace joyeer::llvmbackend::wire

#endif