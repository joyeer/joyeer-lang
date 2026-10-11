#ifndef JOYEER_BACKEND_NATIVE_IR_H
#define JOYEER_BACKEND_NATIVE_IR_H

#include "joyeer/backend/llvm.h"
#include "joyeer/compiler/hostbuiltins.h"
#include "joyeer/native/runtime.h"
#include "native_module.h"

#include "llvm/IR/DIBuilder.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/NoFolder.h"

#include <map>
#include <unordered_map>
#include <unordered_set>

namespace joyeer::llvmbackend::native {

struct Layout {
    size_t size = 0;
    size_t alignment = 1;
};

// One lowering session owns all caches and insertion state. The translation
// units implement separate responsibilities, rather than separate IR models.
class Builder {
public:
    Builder(ModuleStorage& storage, const ir::Module& source, EmitOptions options);
    std::vector<Diagnostic> build();
    bool hasEntryPoint = false;

private:
    ModuleStorage& storage;
    const ir::Module& source;
    EmitOptions options;
    llvm::IRBuilder<llvm::NoFolder> builder;
    std::unique_ptr<llvm::DIBuilder> debug;
    const ir::Function* currentFunction = nullptr;
    const ir::BasicBlock* currentBlock = nullptr;
    llvm::Function* activeFunction = nullptr;
    llvm::BasicBlock* prologue = nullptr;
    llvm::DISubprogram* subprogram = nullptr;
    std::vector<Diagnostic> diagnostics;
    std::unordered_map<ir::TypeId, const ir::TypeName*> types;
    std::unordered_map<ir::TypeId, const ir::StructureDefinition*> structures;
    std::unordered_map<ir::TypeId, const ir::EnumerationDefinition*> enumerations;
    std::unordered_map<ir::TypeId, llvm::Type*> loweredTypes;
    std::unordered_map<ir::TypeId, Layout> layouts;
    std::unordered_set<ir::TypeId> layoutStack;
    std::unordered_map<std::string, llvm::Value*> panicMessages;
    std::unordered_map<ir::FunctionId, llvm::Function*> functions;
    std::unordered_map<ir::ValueId, ir::Value> values;
    std::unordered_map<ir::ValueId, llvm::Value*> operands;
    std::unordered_map<ir::BlockId, llvm::BasicBlock*> blocks;
    std::map<ir::TypeId, llvm::Function*> clones;
    std::map<ir::TypeId, llvm::Function*> destroys;
    std::vector<llvm::DIFile*> debugFiles;
    std::unordered_map<ir::TypeId, llvm::DIType*> debugTypes;
    std::unordered_map<ir::DebugScopeId, llvm::DILocalScope*> debugScopes;
    std::unordered_map<ir::DebugVariableId, llvm::DILocalVariable*> debugVariables;
    std::map<std::pair<llvm::DILocalScope*, uint32_t>, llvm::DILocalScope*> debugFileScopes;
    size_t nextString = 0;
    size_t nextTemporary = 0;

    llvm::LLVMContext& context() { return storage.context; }
    llvm::Module& module() { return *storage.module; }
    llvm::Type* pointer() { return builder.getPtrTy(); }
    llvm::Constant* nullPointer() { return llvm::ConstantPointerNull::get(builder.getPtrTy()); }
    llvm::ConstantInt* integer(uint64_t value, unsigned bits = 64) {
        return llvm::ConstantInt::get(llvm::IntegerType::get(context(), bits), value);
    }
    std::string temporary() { return "tmp" + std::to_string(nextTemporary++); }
    llvm::Value* operand(ir::ValueId id) { return operands.at(id); }
    const ir::TypeName& type(ir::TypeId id) { return *types.at(id); }
    bool unit(ir::TypeId id) { return type(id).kind == typing::TypeKind::voidType; }
    void report(DiagnosticId id, SourceSpan span, std::string message);

    // Types/layout and ownership use Joyeer's portable 64-bit runtime ABI.
    llvm::Type* llvmType(ir::TypeId id);
    Layout layout(ir::TypeId id);
    std::vector<size_t> payloadOffsets(const ir::EnumCaseDefinition& definition);
    void defineTypes();
    llvm::Function* declareFunction(const ir::Function& function);
    llvm::FunctionCallee runtime(std::string_view name, llvm::Type* result,
            llvm::ArrayRef<llvm::Type*> parameters);
    llvm::CallInst* call(std::string_view name, llvm::Type* result,
            llvm::ArrayRef<llvm::Value*> arguments);
    llvm::Value* stack(llvm::Type* type, const llvm::Twine& name = "");
    llvm::Value* spill(llvm::Value* value);
    llvm::Value* field(llvm::Type* type, llvm::Value* address, unsigned index);
    llvm::Value* payload(llvm::Type* type, llvm::Value* address, size_t offset = 0);
    llvm::Value* bytes(llvm::Value* address, llvm::Value* offset);
    llvm::Value* stringLiteral(std::string_view text, bool terminate = false);
    llvm::Value* helper(ir::TypeId id, bool clone);
    void emitOwnership();
    void emitClone(ir::TypeId id);
    void emitDestroy(ir::TypeId id);
    void copy(ir::TypeId id, llvm::Value* destination, llvm::Value* sourceAddress);

    // Functions/instructions preserve SSA IDs while LLVM owns the actual values.
    void emitFunction(const ir::Function& function);
    bool emitInstruction(const ir::Instruction& instruction);
    void emitEntry();
    void trap(llvm::Value* condition, std::string_view message);
    llvm::Value* arithmetic(const ir::Instruction& instruction);
    llvm::Value* comparison(ir::Opcode opcode, llvm::Value* left, llvm::Value* right);
    llvm::Value* constructEnum(ir::TypeId id, size_t tag, llvm::ArrayRef<llvm::Value*> values);
    llvm::Value* extractPayload(ir::TypeId id, llvm::Value* value,
            semantic::SymbolId symbol, size_t index);
    llvm::Value* pattern(const ir::Pattern& pattern, llvm::Value* value);
    llvm::Value* emitCall(const ir::Instruction& instruction);
    bool emitCollection(const ir::Instruction& instruction, llvm::Value*& result);
    int keyKind(ir::TypeId id);
    llvm::Value* dictionaryFind(ir::TypeId id, llvm::Value* dictionary, llvm::Value* key);
    llvm::Value* emitHostCall(const ir::Function& function,
            llvm::ArrayRef<llvm::Value*> arguments, const ir::Instruction& instruction);
        bool matchesHostValue(ir::TypeId id, hostbuiltins::ValueKind kind);
        std::vector<size_t> enumTags(ir::TypeId id, const hostbuiltins::Enumeration& shape);
    llvm::Value* hostEnum(ir::TypeId id, llvm::Value* storageAddress,
            llvm::Value* tag, const hostbuiltins::Enumeration& shape);

    // Debug locations are attached to objects as they are built, never by
    // searching or rewriting printed instruction text.
    bool fullDebug() const { return options.emitLineTables && options.emitVariables; }
    void initializeDebug();
    std::pair<uint32_t, uint32_t> position(SourceSpan span);
    llvm::DIType* debugType(ir::TypeId id);
    llvm::DILocalScope* debugScope(ir::DebugScopeId id);
    llvm::DILocalScope* locationScope(std::optional<ir::DebugScopeId> id, SourceSpan span);
    llvm::DILocation* location(const std::optional<ir::DebugLocation>& location);
    void functionDebug(const ir::Function& function);
    bool bindDebug(const ir::DebugVariableBinding& binding);
};

} // namespace joyeer::llvmbackend::native

#endif