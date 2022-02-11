//
// Created by Qing Xu on 2022/1/3.
//

#include "joyeer/runtime/sys.h"
#include "joyeer/runtime/arguments.h"
#include "joyeer/runtime/executor.h"

Value Global_$_print(Executor* executor, Arguments *args) {
    auto wrappedObj = args->getArgument(0);
    auto head = reinterpret_cast<ObjectHead*>(wrappedObj);
    auto vm = executor->vm;
    auto type = vm->types->operator[](head->typeSlot);
    switch (type->kind) {
        case ValueType::Int: {
            auto optionalInt = (Optional*)vm->getType(BuildIns::Object_Optional_Int);
            auto value = optionalInt->intValue(wrappedObj);
            printf("%lld\n", value);
        }
            break;
        case ValueType::Bool: {
            auto optionalBool = (Optional*)vm->getType(BuildIns::Object_Optional_Bool);
            auto value = optionalBool->boolValue(wrappedObj);
            if(value) {
                printf("true\n");
            } else {
                printf("false\n");
            }
        }
            break;
        case ValueType::String: {
            auto string = executor->vm->stringClass->toString(wrappedObj);
            printf("%s\n", string.c_str());
        }
            break;
        case ValueType::Nil: {
            printf("nil\n");
        }
            break;
        default:
            assert(false);
    }
    return 0;
}

Value Global_$_autoWrapping_Int(Executor* executor, Arguments* args) {
    auto value = args->getArgument(0);
    auto optionalInt = (Optional*)executor->vm->getType(BuildIns::Object_Optional_Int);
    auto wrapped = optionalInt->allocate(executor->vm,(Int)value);
    return wrapped;
}

Value Global_$_autoWrapping_Bool(Executor* executor, Arguments* args) {
    auto value = args->getArgument(0);
    auto optionalBool = (Optional*)executor->vm->getType(BuildIns::Object_Optional_Int);
    auto wrapped = optionalBool->allocate(executor->vm, (Bool)value);
    return wrapped;
}

Value Global_$_autoWrapping_Class(Executor* executor, Arguments* args) {
    auto value = args->getArgument(0);
    auto objectHead = (ObjectHead*)value;

    assert(objectHead->typeSlot != -1);

    auto type = executor->vm->getType(objectHead->typeSlot);
    assert(type->optionalTypeSlot != -1);
    auto optionalType = (Optional*)executor->vm->getType(type->optionalTypeSlot);
    assert(optionalType->kind == ValueType::Optional);
    return optionalType->allocate(executor->vm, value);
}

Value Global_$_autoUnwrapping(Executor* executor, Arguments* args) {
    auto value = args->getArgument(0);
    auto head = (ObjectHead*)value;
    auto type = executor->vm->getType(head->typeSlot);
    assert(type->kind == ValueType::Optional);
    auto optionalClass = (Optional*)type;
    return optionalClass->value(value);
}

Value Array_$$_get(Executor* executor, Arguments *args) {
    auto indexValue = args->getArgument(0);
    auto objValue = args->getArgument(1);

    auto arrayClass = executor->vm->arrayClass;
    assert(arrayClass != nullptr);

    auto itemValue = arrayClass->get(objValue, indexValue);
    return itemValue;
}

Value Array_$$_size(Executor *executor, Arguments *args) {
    auto objValue = args->getArgument(0);
    auto arrayClass = executor->vm->arrayClass;
    assert(arrayClass != nullptr);
    auto resultValue = arrayClass->getLength(objValue);
    return resultValue;
}

Value Array_$$_set(Executor *executor, Arguments *args) {
    auto newValue = args->getArgument(0);
    auto indexValue = args->getArgument(1);
    auto objValue = args->getArgument(2);

    auto arrayClass = executor->vm->arrayClass;
    arrayClass->set(objValue, indexValue, newValue);
    assert(arrayClass != nullptr);

    return 0;
}

Value Dict_$_init(Executor* executor, Arguments* args) {
    auto obj = args->getArgument(0);
    return obj;
}

Value Dict_$$_insert(Executor* executor, Arguments* args) {
    auto value = args->getArgument(0);
    auto keyValue = args->getArgument(1);
    auto objValue = args->getArgument(2);

    auto dictClass = executor->vm->dictClass;
    dictClass->insert(objValue, keyValue, value);
    return 0;
}

Value Dict_$$_get(Executor* executor, Arguments* args) {
    auto keyValue = args->getArgument(0);
    auto objValue = args->getArgument(1);

    auto dictClass = executor->vm->dictClass;
    auto value = dictClass->get(objValue, keyValue);

    return value;
}

Value StringBuilder_$$_append(Executor* executor, Arguments* args) {
    auto value = args->getArgument(0);
    auto objValue = args->getArgument(1);

    auto stringBuilderClass = executor->vm->stringBuilderClass;
    stringBuilderClass->append(objValue, value);
    return objValue;
}

Value StringBuilder_$$_toString(Executor* executor, Arguments* args) {
    auto objValue = args->getArgument(0);
    auto stringBuilderClass = executor->vm->stringBuilderClass;
    return stringBuilderClass->toString(objValue);
}

