//
// Created by Qing Xu on 2022/1/2.
//

#include "joyeer/runtime/isolate+vm.h"

IsolateVM::IsolateVM() {
    gc = new GC();
}

IsolateVM::~IsolateVM() {
    delete gc;
}

Type* IsolateVM::getType(BuildIns type) {
    return (*types)[(int)(type)];
}

Type* IsolateVM::getType(Slot slot) {
    return (*types)[(int)slot];
}

void IsolateVM::bootstrap() {
    arrayClass = (ArrayClass*)((*types)[(int) BuildIns::Object_Array]);
    dictClass = (DictClass*)((*types)[(int) BuildIns::Object_Dict]);
    dictEntryClass = (DictEntry*)((*types)[(int) BuildIns::Object_DictEntry]);
    stringClass = (StringClass*)((*types)[(int) ValueType::String]);
    stringBuilderClass = (StringBuilderClass*)((*types)[(int) BuildIns::Object_StringBuilder]);
    stringBuilderClass->vm = this;
}