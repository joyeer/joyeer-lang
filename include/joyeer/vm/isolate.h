//
// Created by Qing Xu on 2021/11/8.
//

#ifndef __joyeer_vm_isolate_h__
#define __joyeer_vm_isolate_h__

#include "joyeer/runtime/gc.h"
#include "joyeer/runtime/res+table.h"
#include "joyeer/runtime/isolate+vm.h"
#include "joyeer/compiler/compiler+service.h"

struct InterpretedIsolatedVM : public IsolateVM {
    void run(ModuleClass* module);
};

#endif //__joyeer_vm_isolate_h__
