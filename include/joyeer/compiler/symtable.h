#ifndef __joyeer_compiler_syntax_symtable_h__
#define __joyeer_compiler_syntax_symtable_h__

#include <unordered_map>
#include <memory>


#include "joyeer/runtime/types.h"

enum class SymbolFlag : uint16_t {
    func = 1,
    constructor,
    var,
    field,
    klass,
};

std::string debugStringOfSymbolFlag(SymbolFlag flag);

/**
 * Symbol contains information about
 * 1. name
 * 2. kind, which kind of symbol, and declared in SymbolFlag
 * 3. Type
 * 4. Address
 */
struct Symbol {
    using Ptr = std::shared_ptr<Symbol>;

    SymbolFlag flag;
    std::string name;

    // typeSlot of ModuleClass/Function/Class
    int typeSlot:32 = -1;

    // the parentTypeSlot type's slot
    int parentTypeSlot:32 = -1;

    int locationInParent:16 = -1;

    bool isStatic = false;

    Symbol(SymbolFlag flag, std::string  name, int address);
};


class SymbolTable {
public:
    typedef std::shared_ptr<SymbolTable> Ptr;
    
public:
    SymbolTable();
    
    // insert an new Symbol
    bool insert(const Symbol::Ptr& symbol);
    
    // find symbol by a given name
    [[nodiscard]] Symbol::Ptr find(const std::string& name) const;

    std::unordered_map<std::string, Symbol::Ptr>::const_iterator begin();
    std::unordered_map<std::string, Symbol::Ptr>::const_iterator end();
private:
    friend class NodeDebugPrinter;
    std::unordered_map<std::string, Symbol::Ptr> symbols;
};

#endif
