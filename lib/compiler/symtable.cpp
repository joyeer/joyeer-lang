#include "joyeer/compiler/symtable.h"
#include "joyeer/diagnostic/diagnostic.h"
#include <utility>
#include <cassert>


std::string debugStringOfSymbolFlag(SymbolFlag flag) {
    switch (flag) {
        case SymbolFlag::var:
            return "var";
        case SymbolFlag::func:
            return "func";
        case SymbolFlag::field:
            return "field";
        case SymbolFlag::klass:
            return "class";
        case SymbolFlag::constructor:
            return "constructor";
        default:
            assert(false);
    }
    return nullptr;
}

Symbol::Symbol(SymbolFlag flag, std::string name, int address):
        flag(flag),
        name(std::move(name)),
        typeSlot(address) {
}

/////////////////////////////////////////////////////////////////
// SymTable
/////////////////////////////////////////////////////////////////
SymbolTable::SymbolTable() :
symbols() {
}

bool SymbolTable::insert(const Symbol::Ptr& symbol) {
    if(symbols.find(symbol->name) != symbols.end()) {
        assert(false);
        return false;
    }

    symbol->locationInParent = symbols.size();
    symbols.insert({
        symbol->name,
        symbol
    });
    
    return true;
}

Symbol::Ptr SymbolTable::find(const std::string& name) const {
    auto result = symbols.find(name);
    if( result != symbols.end()) {
        return result->second;
    }
    
    return nullptr;
}

std::unordered_map<std::string, Symbol::Ptr>::const_iterator SymbolTable::begin() {
    return symbols.begin();
}

std::unordered_map<std::string, Symbol::Ptr>::const_iterator SymbolTable::end() {
    return symbols.end();
}
