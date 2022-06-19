//
// Created by Qing Xu on 2022/1/2.
//

#include "joyeer/runtime/res+table.h"
#include <cassert>

int StringResTable::import(const std::string &string) {
    strings.push_back(string);
    return strings.size() - 1;
}

const std::string &StringResTable::operator[](int index) {
    assert(index > -1 && index < strings.size());
    return strings[index];
}

Type* TypeResTable::operator[](int index) {
    assert(index > -1 && index < types.size());
    return types[index];
}

Type *TypeResTable::operator[](ValueType valueType) {
    return operator[]((int)valueType);
}

int TypeResTable::import(Type *type) {
    assert(type->slot == -1);
    types.push_back(type);
    type->slot = types.size() - 1;
    return type->slot;
}
