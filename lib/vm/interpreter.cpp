//
// Created by Qing Xu on 2021/11/7.
//

#include "joyeer/vm/interpreter.h"
#include "joyeer/runtime/bytecode.h"
#include "joyeer/runtime/types.h"
#include "joyeer/compiler/debugprinter.h"
#include <iostream>

struct Interpreter;

#define DEBUG_PRINT(bytecode, types) \
    std::cout << ">" << debugPrint(bytecode, types) << std::endl;

#define HANDLE_BYTECODE(OP) \
        case OP_##OP: \
            Handle_##OP(bytecode); \
            goto next;

#define HANDLE_BYTECODE_RETURN(OP) \
        case OP_##OP: \
            Handle_##OP(bytecode); \
            goto exit;

struct Interpreter {
    AddressPtr cp = 0;
    InterpretedExecutor* executor;
    IsolateVM* isolateVm;
    Bytecodes* bytecodes;

    Interpreter(InterpretedExecutor* executor, Bytecodes* bytecodes) :
            executor(executor),
            bytecodes(bytecodes) {
        isolateVm = executor->vm;
    }

    [[maybe_unused]] void run() {
        assert(bytecodes != nullptr);
        assert(bytecodes->bytecodes != nullptr);
loop:
        auto bytecode = *(Bytecode *)(bytecodes->bytecodes + cp);

        auto opcode = OP_FROM_BYTECODE(bytecode);
        DEBUG_PRINT(bytecode, isolateVm->types->types)

        switch (opcode) {
            HANDLE_BYTECODE(NOP)
            HANDLE_BYTECODE(OCONST_NIL)
            HANDLE_BYTECODE(ICONST)
            HANDLE_BYTECODE(SCONST)
            HANDLE_BYTECODE(ISTORE)
            HANDLE_BYTECODE(OLOAD)
            HANDLE_BYTECODE(ILOAD)
            HANDLE_BYTECODE(NEW)
            HANDLE_BYTECODE(PUTFIELD)
            HANDLE_BYTECODE(GETFIELD)
            HANDLE_BYTECODE(PUTSTATIC)
            HANDLE_BYTECODE(GETSTATIC)
            HANDLE_BYTECODE(IAND)
            HANDLE_BYTECODE(ICMP_G)
            HANDLE_BYTECODE(ICMP_GE)
            HANDLE_BYTECODE(ICMP_L)
            HANDLE_BYTECODE(ICMP_LE)
            HANDLE_BYTECODE(ICMP_NE)
            HANDLE_BYTECODE(ICMP_EQ)
            HANDLE_BYTECODE(IADD)
            HANDLE_BYTECODE(ISUB)
            HANDLE_BYTECODE(IMUL)
            HANDLE_BYTECODE(IDIV)
            HANDLE_BYTECODE(IREM)
            HANDLE_BYTECODE(INEG)
            HANDLE_BYTECODE(IFEQ)
            HANDLE_BYTECODE(IFNE)
            HANDLE_BYTECODE(IFLT)
            HANDLE_BYTECODE(IFLE)
            HANDLE_BYTECODE(IFGT)
            HANDLE_BYTECODE(IFGE)
            HANDLE_BYTECODE_RETURN(RETURN)
            HANDLE_BYTECODE_RETURN(IRETURN)
            HANDLE_BYTECODE_RETURN(ORETURN)
            HANDLE_BYTECODE(ONEWARRAY)
            HANDLE_BYTECODE(INVOKE)
            HANDLE_BYTECODE(DUP)
            HANDLE_BYTECODE(POP)
            HANDLE_BYTECODE(GOTO)
            HANDLE_BYTECODE(DEBUG)
            default:
                assert(false);

        }
    next:
        cp += kIntSize;
        goto loop;

    exit:
        return;
    }

    inline void push(Value value) const {
        executor->push(value);
    }

    inline Value pop() {
        return executor->pop();
    }

    inline Value top() {
        return executor->top();
    }

    inline void Handle_NOP(Bytecode bytecode) {
        assert(OP_FROM_BYTECODE(bytecode) == OP_NOP);
        assert(false);
    }

    inline void Handle_OCONST_NIL(Bytecode bytecode) {
        assert(OP_FROM_BYTECODE(bytecode) == OP_OCONST_NIL);
        push(NilType::nilValue());
    }

    inline void Handle_ICONST(Bytecode bytecode) {
        assert(OP_FROM_BYTECODE(bytecode) == OP_ICONST);
        auto value = VALUE_FROM_BYTECODE(bytecode);
        push(value);
    }

    inline void Handle_SCONST(Bytecode bytecode) {
        assert(OP_FROM_BYTECODE(bytecode) == OP_SCONST);
        auto value = VALUE_FROM_BYTECODE(bytecode);


        auto stringObj = isolateVm->stringClass->allocate(isolateVm, value);
        push(stringObj);
    }

    inline void Handle_ISTORE(Bytecode bytecode) {
        assert(OP_FROM_BYTECODE(bytecode) == OP_ISTORE);
        auto slot = VALUE_FROM_BYTECODE(bytecode);
        auto newValue = pop();

        FuncCallFrame::setLocalVar(executor->getCurrentFrame(), slot, newValue);
    }

    inline void Handle_OLOAD(Bytecode bytecode) {
        assert(OP_FROM_BYTECODE(bytecode) == OP_OLOAD);
        auto value = VALUE_FROM_BYTECODE(bytecode);
        auto objValue = FuncCallFrame::getLocalVar(executor->getCurrentFrame(), value);
        push(objValue);
    }

    inline void Handle_ILOAD(Bytecode bytecode) {
        assert(OP_FROM_BYTECODE(bytecode) == OP_ILOAD);
        auto value = VALUE_FROM_BYTECODE(bytecode);

        auto intValue = FuncCallFrame::getLocalVar(executor->getCurrentFrame(), value);
        push(intValue);
    }

    inline void Handle_NEW(Bytecode bytecode) {
        assert(OP_FROM_BYTECODE(bytecode) == OP_NEW);

        auto classTypeSlot = VALUE_FROM_BYTECODE(bytecode);
        auto klass = (Class*)(*executor->vm->types)[classTypeSlot];
        auto obj = klass->allocate(executor->vm);
        push(obj);
    }

    inline void Handle_PUTFIELD(Bytecode bytecode) {
        assert(OP_FROM_BYTECODE(bytecode) == OP_PUTFIELD);
        auto fieldOffset = VALUE_FROM_BYTECODE(bytecode);
        auto object = pop();
        auto value = pop();
        auto objectHead = (ObjectHead *) object;
        auto classTypeSlot = objectHead->typeSlot;
        auto klass = (Class*)(*executor->vm->types)[classTypeSlot];
        klass->putField(object, fieldOffset, value);
    }

    inline void Handle_GETFIELD(Bytecode bytecode) {
        assert(OP_FROM_BYTECODE(bytecode) == OP_GETFIELD);
        auto object = pop();
        auto fieldOffset = VALUE_FROM_BYTECODE(bytecode);

        auto objectHead = (ObjectHead *) object;
        auto classTypeSlot = objectHead->typeSlot;
        auto klass = (Class*)(*executor->vm->types)[classTypeSlot];
        auto value = klass->getField(object, (int)fieldOffset);
        push(value);
    }

    inline void Handle_PUTSTATIC(Bytecode bytecode) {
        assert(OP_FROM_BYTECODE(bytecode) == OP_PUTSTATIC);

        auto classSlotId = VAL1_FROM_BYTECODE(bytecode);    // slot id of class
        auto fieldOffset = VAL2_FROM_BYTECODE(bytecode); // field variable position
        auto value = pop();

        auto klass = (Class*)(*isolateVm->types)[classSlotId];
        GC::write(klass->staticArea, value, fieldOffset);
    }

    inline void Handle_GETSTATIC(Bytecode bytecode) {
        assert(OP_FROM_BYTECODE(bytecode) == OP_GETSTATIC);
        auto classSlotId = VAL1_FROM_BYTECODE(bytecode);    // slot id of class
        auto fieldOffset = VAL2_FROM_BYTECODE(bytecode);

        auto klass = (Class*)(*isolateVm->types)[classSlotId];
        auto value = GC::read(klass->staticArea, fieldOffset);

        push(value);
    }

    inline void Handle_IAND(Bytecode bytecode) {
        assert(OP_FROM_BYTECODE(bytecode) == OP_IAND);
        auto rightValue = pop();
        auto leftValue = pop();

        push(leftValue > 0 && rightValue > 0 );
    }

    inline void Handle_ICMP_G(Bytecode bytecode) {
        assert(OP_FROM_BYTECODE(bytecode) == OP_ICMP_G);
        auto rightValue = pop();
        auto leftValue = pop();
        if(leftValue > rightValue ) {
            push(1);
        } else {
            push(0);
        }
    }

    inline void Handle_ICMP_GE(Bytecode bytecode) {
        assert(OP_FROM_BYTECODE(bytecode) == OP_ICMP_GE);
        auto rightValue = pop();
        auto leftValue = pop();
        if(leftValue >= rightValue ) {
            push(1);
        } else {
            push(0);
        }
    }

    inline void Handle_ICMP_L(Bytecode bytecode) {
        assert(OP_FROM_BYTECODE(bytecode) == OP_ICMP_L);
        auto rightValue = pop();
        auto leftValue = pop();
        if(leftValue < rightValue ) {
            push(1);
        } else {
            push(0);
        }
    }

    inline void Handle_ICMP_LE(Bytecode bytecode) {
        assert(OP_FROM_BYTECODE(bytecode) == OP_ICMP_LE);
        auto rightValue = pop();
        auto leftValue = pop();
        if(leftValue <= rightValue) {
            push(1);
        } else {
            push(0);
        }
    }

    inline void Handle_ICMP_NE(Bytecode bytecode) {
        assert(OP_FROM_BYTECODE(bytecode) == OP_ICMP_NE);
        auto rightValue = pop();
        auto leftValue = pop();
        if(leftValue != rightValue ) {
            push(1);
        } else {
            push(0);
        }
    }

    inline void Handle_ICMP_EQ(Bytecode bytecode) {
        assert(OP_FROM_BYTECODE(bytecode) == OP_ICMP_EQ);
        auto rightValue = pop();
        auto leftValue = pop();
        if(leftValue == rightValue ) {
            push(1);
        } else {
            push(0);
        }
    }

    inline void Handle_IADD(Bytecode bytecode) {
        assert(OP_FROM_BYTECODE(bytecode) == OP_IADD);

        auto value1 = pop();
        auto value2 = pop();

        auto result = value2 + value1;
        push(result);
    }

    inline void Handle_ISUB(Bytecode bytecode) {
        assert(OP_FROM_BYTECODE(bytecode) == OP_ISUB);
        auto value1 = pop();
        auto value2 = pop();

        auto result = value2 - value1;
        push(result);
    }

    inline void Handle_IMUL(Bytecode bytecode) {
        assert(OP_FROM_BYTECODE(bytecode) == OP_IMUL);
        auto value1 = pop();
        auto value2 = pop();

        auto result = value2 * value1;
        push(result);
    }

    inline void Handle_IDIV(Bytecode bytecode) {
        assert(OP_FROM_BYTECODE(bytecode) == OP_IDIV);
        auto value1 = pop();
        auto value2 = pop();

        auto result = value2 / value1;
        push(result );
    }

    inline void Handle_IREM(Bytecode bytecode) {
        assert(OP_FROM_BYTECODE(bytecode) == OP_IREM);
        auto value1 = pop();
        auto value2 = pop();

        auto result = value2 % value1;
        push(result);
    }

    inline void Handle_INEG(Bytecode bytecode) {
        assert(OP_FROM_BYTECODE(bytecode) == OP_INEG);

        auto value = pop();

        auto result = 0 - value;
        push(result);
    }

    inline void Handle_IFEQ(Bytecode bytecode) {
        assert(OP_FROM_BYTECODE(bytecode) == OP_IFEQ);
        assert(false);
    }

    inline void Handle_IFNE(Bytecode bytecode) {
        assert(OP_FROM_BYTECODE(bytecode) == OP_IFNE);
        auto value = pop();
        auto offset = VALUE_FROM_BYTECODE(bytecode);

        if (value == 0) {
            cp += offset * kIntSize;
        }
    }

    inline void Handle_IFLT(Bytecode bytecode) {
        assert(OP_FROM_BYTECODE(bytecode) == OP_IFLT);
        assert(false);
    }

    inline void Handle_IFLE(Bytecode bytecode) {
        assert(OP_FROM_BYTECODE(bytecode) == OP_IFLE);
        auto value = pop();
        auto offset = VALUE_FROM_BYTECODE(bytecode);

        if(value <= 0 ) {
            cp += offset * kIntSize;
        }
    }

    inline void Handle_IFGT(Bytecode bytecode) {
        assert(OP_FROM_BYTECODE(bytecode) == OP_IFGT);
        assert(false);
    }

    inline void Handle_IFGE(Bytecode bytecode) {
        assert(OP_FROM_BYTECODE(bytecode) == OP_IFGE);
        assert(false);
    }

    inline void Handle_RETURN(Bytecode bytecode) {
        assert(OP_FROM_BYTECODE(bytecode) == OP_RETURN);
    }

    inline void Handle_IRETURN(Bytecode bytecode) {
        assert(OP_FROM_BYTECODE(bytecode) == OP_IRETURN);
        auto value = pop();

        FuncCallFrame::setReturnValue(executor->getCurrentFrame(), value);
    }

    inline void Handle_ORETURN(Bytecode bytecode) {
        assert(OP_FROM_BYTECODE(bytecode) == OP_ORETURN);
        auto value = pop();
        FuncCallFrame::setReturnValue(executor->getCurrentFrame(), value);
    }

    inline void Handle_ONEWARRAY(Bytecode bytecode) {
        assert(OP_FROM_BYTECODE(bytecode) == OP_ONEWARRAY);
        auto arrayClass = isolateVm->arrayClass;
        auto value = pop();
        auto arrayObj = arrayClass->allocate(isolateVm, value);
        for(int i = 0 ; i < value; i ++ ) {
            auto popValue = pop();
            arrayClass->set(arrayObj, value - i - 1, popValue);
        }
        arrayClass->setLength(arrayObj, value);
        push(arrayObj);
    }

    inline void Handle_INVOKE(Bytecode bytecode) {
        assert(OP_FROM_BYTECODE(bytecode) == OP_INVOKE);
        auto methodSlot = VALUE_FROM_BYTECODE(bytecode);
        auto method = (Function*)(*isolateVm->types)[methodSlot];
        executor->execute(method);
    }

    inline void Handle_DUP(Bytecode bytecode) {
        assert(OP_FROM_BYTECODE(bytecode) == OP_DUP);
        auto r = top();
        push(r);
    }

    void Handle_POP(Bytecode bytecode) {
        assert(OP_FROM_BYTECODE(bytecode) == OP_POP);
        pop();
    }

    inline void Handle_GOTO(Bytecode bytecode) {
        assert(OP_FROM_BYTECODE(bytecode) == OP_GOTO);

        auto offset = VALUE_FROM_BYTECODE(bytecode);
        cp += offset * kIntSize;
    }

    inline void Handle_DEBUG(Bytecode bytecode) {
        assert(OP_FROM_BYTECODE(bytecode) == OP_DEBUG);
        assert(false);
    }
};

InterpretedExecutor::InterpretedExecutor(IsolateVM *vm) {
    this->vm = vm;
}

void InterpretedExecutor::execute(const ModuleClass *module) {
    auto savedFP = sp;
    auto frame = &stack[sp];
    ModuleEntryFrame::set(frame, 0, module->slot);

    push(savedFP, ModuleEntryFrame::size());
    auto function = (Function*)(*vm->types)[module->staticInitializerSlot];
    execute(function);
    pop(savedFP);
}

void InterpretedExecutor::execute(const Function *function) {

    auto paramCount = function->paramCount;

    auto savedFP = sp;

    push(savedFP, FuncCallFrame::size());

    switch (function->funcType) {
        case FuncType::C_Func:
        case FuncType::C_CInit:
            invokeCFunction(function);
            break;
        case FuncType::VM_CInit:
        case FuncType::VM_Func:
            invokeVFunction(function);
            break;
        default:
            assert(false);
    }

    auto resultValue = FuncCallFrame::getReturnValue(getCurrentFrame());

    pop(savedFP);

    sp = sp - paramCount * kValueSize - (function->isStatic ? 0 : kValueSize);

    if(function->returnTypeSlot != (int)ValueType::Void ) {
        push(resultValue);
    }

}

FramePtr InterpretedExecutor::getCurrentFrame() const {
    return (FramePtr )(stack + fp);
}

void InterpretedExecutor::invokeVFunction(const Function *function){
    // prepare the variable
    Arguments arguments(this);

    for(int i = 0; i < function->getLocalVarCount(); i ++ ) {
        if(i < function->paramCount) {
            auto argument = arguments.getArgument(function->paramCount - i - 1);
            push(argument);
        } else {
            push(0);
        }
    }
    Interpreter interpreter(this, function->bytecodes);
    interpreter.run();
}

void  InterpretedExecutor::invokeCFunction(const Function *function) {
    Arguments arguments(this);

    auto cFunction = *(function->cFunction);
    auto cReturnValue = cFunction(this, reinterpret_cast<Argument *>(&arguments));
    FuncCallFrame::setReturnValue(getCurrentFrame(), cReturnValue);
}

void InterpretedExecutor::push(Slot frame, int size) {
    frames.push_back(frame);
    sp += size;
    fp = frame;
}

void InterpretedExecutor::pop(Slot frame) {
    assert(frames.back() == frame);
    frames.pop_back();
    sp = frame;
    if(frames.size() > 0) {
        fp = frames.back();
    } else {
        fp = 0;
    }
}

void InterpretedExecutor::push(Value value) {
    *(Int *)(stack + sp) = value;
    sp += kValueSize;
}

Value InterpretedExecutor::pop() {
    sp -= kValueSize;
    auto result = *(Int *)(stack + sp);

    return result;
}

Value InterpretedExecutor::top() const {
    auto r = *(Int *)(stack + sp - kValueSize);
    return r;
}
