//
// Created by Qing Xu on 2022/1/2.
//

#ifndef __joyeer_isolate_vm_h__
#define __joyeer_isolate_vm_h__

#include "joyeer/runtime/res+table.h"
#include "joyeer/runtime/gc.h"
#include "joyeer/runtime/sys.h"

struct IsolateVM {

    IsolateVM();
    virtual ~IsolateVM();

    void bootstrap();

    StringResTable* strings { new StringResTable() };
    TypeResTable* types { new TypeResTable() };

    Type* getType(BuildIns type);
    Type* getType(Slot slot);

    GC* gc;

    ArrayClass* arrayClass = nullptr;
    DictClass* dictClass = nullptr;
    DictEntry* dictEntryClass = nullptr;
    StringClass* stringClass = nullptr;
    StringBuilderClass* stringBuilderClass = nullptr;
};


#endif //__joyeer_isolate_vm_h__
