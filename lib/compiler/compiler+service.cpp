#include "joyeer/compiler/compiler+service.h"

#include "joyeer/compiler/lexparser.h"
#include "joyeer/compiler/typegen.h"
#include "joyeer/compiler/typebinding.h"
#include "joyeer/compiler/syntaxparser.h"
#include "joyeer/compiler/IRGen.h"
#include "debugprinter.h"
#include "joyeer/runtime/types.h"
#include "joyeer/runtime/sys.h"

#include <utility>


#define CHECK_ERROR_RETURN_NULL \
    if(diagnostics->errors.size() != 0) { \
        return nullptr; \
    }

CompilerService::CompilerService(Diagnostics* diagnostics, CommandLineArguments::Ptr opts):
        options(std::move(opts)) {
    globalSymbols = std::make_shared<SymbolTable>();
    this->diagnostics = diagnostics;
}

ModuleClass* CompilerService::compile(const std::string& inputFile) {
    auto sourcefile = findSourceFile(inputFile);
    return compile(sourcefile);
}

SourceFile::Ptr CompilerService::findSourceFile(const std::string &path, const std::string& relativeFolder) {
    auto sourcefile = std::filesystem::path(path);
    if(sourcefile.is_relative()) {
        // check the relative folder
        auto target = std::filesystem::path(relativeFolder) / path;
        if(std::filesystem::exists(target)) {
            sourcefile = target;
        }
    }
    
    auto sourcefilePath = sourcefile.string();
    if(sourceFiles.find(sourcefilePath) == sourceFiles.end()) {
        auto sf = std::make_shared<SourceFile>(options->workingDirectory, sourcefilePath);
        sourceFiles.insert({sourcefilePath, sf});
    }
    
    return sourceFiles.find(sourcefilePath)->second;
}


ModuleClass* CompilerService::compile(const SourceFile::Ptr& sourcefile) {

    auto debugfile = sourcefile->getAbstractLocation() + ".xdump.yml";
    NodeDebugPrinter debugPrinter(debugfile);

    auto context= std::make_shared<CompileContext>(diagnostics, globalSymbols);
    context->sourcefile = sourcefile;
    context->compiler = this;
    
    // lex structure analyze
    LexParser lexParser(context);
    lexParser.parse(sourcefile);
    CHECK_ERROR_RETURN_NULL
    
    // syntax analyze
    SyntaxParser syntaxParser(context, sourcefile);
    auto block = syntaxParser.parse();
    CHECK_ERROR_RETURN_NULL
    debugPrinter.print("parsing-stage", block);

    // gen the type from source code
    TypeGen typeGen(context);
    typeGen.visit(block);
    CHECK_ERROR_RETURN_NULL
    debugPrinter.print("typegen-stage", block);

    // binding the types
    TypeBinding typeBinding(context);
    typeBinding.visit(std::static_pointer_cast<Node>(block));
    CHECK_ERROR_RETURN_NULL
    debugPrinter.print("typebinding-stage", block);

    // generate IR code
    IRGen irGen(context);
    sourcefile->moduleClass = irGen.emit(block);
    CHECK_ERROR_RETURN_NULL

    // debug print the types after IR code generated
    debugPrinter.print("IRGEN-stage", types->types);

    debugPrinter.close();

    return sourcefile->moduleClass;
}

int CompilerService::declare(Type* type) {
    type->slot = static_cast<int32_t>(types->types.size());
    types->types.push_back(type);
    return type->slot;
}

Type* CompilerService::getType(int address) {
    assert(address >= 0 && address < types->types.size());
    return (*types)[address];
}

Type* CompilerService::getType(ValueType valueType) {
    return getType(static_cast<int>(valueType));
}

Type* CompilerService::getType(BuildIns buildIn) {
    return getType(static_cast<int>(buildIn));
}

SymbolTable::Ptr CompilerService::getExportingSymbolTable(int typeSlot) {
    assert(typeSlot != -1);
    return exportingSymbolTableOfClasses[typeSlot];
}

void CompilerService::exportClassDecl(const ClassDecl::Ptr &decl) {
    assert(decl->typeSlot != -1);
    assert(decl->symtable != nullptr);
    assert(!exportingSymbolTableOfClasses.contains(decl->typeSlot));
    exportingSymbolTableOfClasses[decl->typeSlot] = decl->symtable;
}

void CompilerService::registerOptionalTypeGlobally(const Optional *type) {
    assert(type->slot == -1);
    assert(type->wrappedTypeSlot != -1);
    assert(globalSymbols->find(type->name) == nullptr);
    declare((Type*)type);

    // bi-binding the optionalTypeSlot to original type
    auto wrappedType = getType(type->wrappedTypeSlot);
    assert(wrappedType->optionalTypeSlot == -1);
    wrappedType->optionalTypeSlot = type->slot;

    globalSymbols->insert(std::make_shared<Symbol>(SymbolFlag::klass, type->name, type->slot));
}

#define DECLARE_TYPE(type, TypeClass) \
    {                                 \
        auto typeClass = new TypeClass(); \
        declare(typeClass);           \
        assert((type) == types->types.back()->kind); \
        assert((size_t)(type) == (types->types.size() - 1)); \
        globalSymbols->insert(std::make_shared<Symbol>(SymbolFlag::klass, typeClass->name, typeClass->slot)); \
    }

#define BEGIN_DECLARE_FUNC(type, descriptor, retTypeSlot, cFuncImpl) \
    {                                                                \
        auto func = new Function(descriptor, true);                  \
        declare(func);                                               \
        globalSymbols->insert(std::make_shared<Symbol>(SymbolFlag::func, func->name, func->slot)); \
        func->funcType = FuncType::C_Func;                           \
        func->cFunction = (CFunction)&(cFuncImpl);                   \
        func->returnTypeSlot = (int)(retTypeSlot);                   \
        func->paramCount = 0;

#define DECLARE_FUNC_PARM(name, type) \
    {                                 \
        auto variable = new Variable(name); \
        variable->typeSlot = (int)(type);   \
        variable->loc = func->paramCount;   \
        func->localVars.push_back(variable);\
        func->paramCount ++;          \
    }

#define END_DECLARE_FUNC() \
    }

#define BEGIN_DECLARE_CLASS(type, TypeClass) \
    {                                        \
        auto symtable = std::make_shared<SymbolTable>(); \
        auto typeClass = new TypeClass();    \
        declare(typeClass);                  \
        globalSymbols->insert(std::make_shared<Symbol>(SymbolFlag::klass, typeClass->name, typeClass->slot)); \
        assert((size_t)(type) == typeClass->slot);       \
        exportingSymbolTableOfClasses.insert({ typeClass->slot, symtable });

#define DECLARE_CLASS_FUNC(name, typeSlot, cParamCount, cFuncImpl) \
    {                                                              \
        auto func = new Function(name, false);                     \
        func->funcType = FuncType::C_Func;                     \
        func->paramCount = cParamCount;                            \
        func->cFunction = (CFunction)&(cFuncImpl);                 \
        func->returnTypeSlot = (int)(typeSlot);                    \
        auto funcAddress = declare(func);                          \
        symtable->insert(std::make_shared<Symbol>(SymbolFlag::func, name, funcAddress)); \
    }

#define DECLARE_CLASS_INIT(name, typeSlot, cParamCount, cFuncImpl) \
    {                                                              \
        auto func = new Function(name, false);                     \
        func->funcType = FuncType::C_CInit;                    \
        func->paramCount = cParamCount;                            \
        func->cFunction = (CFunction)&(cFuncImpl);                 \
        func->returnTypeSlot = (int)(typeSlot);                    \
        auto funcAddress = declare(func);                          \
        symtable->insert(std::make_shared<Symbol>(SymbolFlag::func, name, funcAddress)); \
    }

#define END_DECLARE_CLASS(type) \
    }


#define BEGIN_DECLARE_OPTIONAL(type, originalTypeSlot) \
    {                                                 \
        auto symtable = std::make_shared<SymbolTable>(); \
        auto originalType = getType(originalTypeSlot); \
        auto name =  "Optional<" + getType(originalTypeSlot)->name + ">";\
        auto typeClass = new Optional(name, (int)(originalTypeSlot));    \
        registerOptionalTypeGlobally(typeClass);

#define END_DECLARE_OPTIONAL }


void CompilerService::bootstrap() {


    DECLARE_TYPE(ValueType::Nil, NilType)
    DECLARE_TYPE(ValueType::Unspecified, UnspecifiedType)
    DECLARE_TYPE(ValueType::Void, VoidType)
    DECLARE_TYPE(ValueType::Int, IntType)
    DECLARE_TYPE(ValueType::Bool, BoolType)
    DECLARE_TYPE(ValueType::Any, AnyType)
    DECLARE_TYPE(ValueType::String, StringClass)

    BEGIN_DECLARE_FUNC(BuildIns::Func_Print, "print(message:)", ValueType::Void, Global_$_print)
        DECLARE_FUNC_PARM("message", ValueType::Any)
    END_DECLARE_FUNC()

    BEGIN_DECLARE_FUNC(BuildIns::Func_AutoWrapping_Int, "autoWrapping(int:)", ValueType::Any, Global_$_autoWrapping_Int)
        DECLARE_FUNC_PARM("int", ValueType::Int)
    END_DECLARE_FUNC()

    BEGIN_DECLARE_FUNC(BuildIns::Func_AutoWrapping_Bool, "autoWrapping(bool:)", ValueType::Any, Global_$_autoWrapping_Bool)
        DECLARE_FUNC_PARM("bool", ValueType::Bool)
    END_DECLARE_FUNC()

    BEGIN_DECLARE_FUNC(BuildIns::Func_autoWrapping_Class, "autoWrapping(class:)", ValueType::Class, Global_$_autoWrapping_Class)
        DECLARE_FUNC_PARM("class", ValueType::Any)
    END_DECLARE_FUNC()

    BEGIN_DECLARE_FUNC(BuildIns::Func_autoUnwrapping, "autoUnwrapping(value:)", ValueType::Int, Global_$_autoUnwrapping)
        DECLARE_FUNC_PARM("value", BuildIns::Object_Optional_Int)
    END_DECLARE_FUNC()

    BEGIN_DECLARE_OPTIONAL(BuildIns::Object_Optional_Int, ValueType::Int)
    END_DECLARE_OPTIONAL

    BEGIN_DECLARE_OPTIONAL(BuildIns::Object_Optional_Bool, ValueType::Bool)
    END_DECLARE_OPTIONAL

    // Declare build-in classes and its members
    BEGIN_DECLARE_CLASS(BuildIns::Object_Array, ArrayClass)
        DECLARE_CLASS_FUNC("size()", ValueType::Int, 0, Array_$$_size)
        DECLARE_CLASS_FUNC("get(index:)", ValueType::Any, 1, Array_$$_get)
        DECLARE_CLASS_FUNC("set(index:value:)", ValueType::Void, 2, Array_$$_set)
    END_DECLARE_CLASS(BuildIns::Object_Array)

    BEGIN_DECLARE_CLASS(BuildIns::Object_Dict, DictClass)
        DECLARE_CLASS_INIT("Dict()", BuildIns::Object_Dict, 0, Dict_$_init)
        DECLARE_CLASS_FUNC("insert(key:value:)", ValueType::Void, 2, Dict_$$_insert)
        DECLARE_CLASS_FUNC("get(key:)", BuildIns::Object_Optional_Int, 1, Dict_$$_get)
    END_DECLARE_CLASS(BuildIns::Object_Dict)

    BEGIN_DECLARE_CLASS(BuildIns::Object_DictEntry, DictEntry)
    END_DECLARE_CLASS(BuildIns::Object_Dict)

    BEGIN_DECLARE_CLASS(BuildIns::Object_StringBuilder, StringBuilderClass)
        DECLARE_CLASS_FUNC("append(string:)", BuildIns::Object_StringBuilder, 1, StringBuilder_$$_append)
        DECLARE_CLASS_FUNC("toString()", ValueType::String, 0, StringBuilder_$$_toString)
    END_DECLARE_CLASS(BuildIns::Object_StringBuilder)

}
