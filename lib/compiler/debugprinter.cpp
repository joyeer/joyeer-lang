#include "joyeer/compiler/debugprinter.h"
#include <fstream>

YMLPrinter::YMLPrinter(const std::string &filename) {
    output.open(filename);
}

void YMLPrinter::printTab() {
    output << std::string(size*2, ' ') ;
}

void YMLPrinter::incTab() {
    size ++;
}

void YMLPrinter::decTab() {
    size --;
}

void YMLPrinter::close() {
    output.flush();
    output.close();
}

//---------------------------------------------
// TypeGenSelfForMemberFunc
//---------------------------------------------


NodeDebugPrinter::NodeDebugPrinter(const std::string& filename):
    YMLPrinter(filename) {
}

Node::Ptr NodeDebugPrinter::visit(const ClassDecl::Ptr& decl) {
    output << "class-decl:";
    DEBUG_BLOCK_START
    output << "class-name: " << decl->getSimpleName();
    newline();
    output << "statements:";
    DEBUG_BLOCK_START
    auto i = 0;
    for(const auto& statement: decl->statements) {
        if(i > 0) {
            newline();
        }
        output << "- ";
        NodeVisitor::visit(statement);
        i++;
    }

    DEBUG_BLOCK_END
    DEBUG_BLOCK_END
    return decl;
}

void NodeDebugPrinter::print(Function *func, const std::vector<Type*>& types)  {
    output<< "name: " << func->name;
    newline();
    output << "param-count: " << func->paramCount;
    if (func->localVars.size() > 0) {
        newline();
        output << "local-vars:";
        for (auto i = 0 ; i < func->localVars.size(); i ++ ) {
            newline();
            output << "- name: " << func->localVars[i]->name;
        }
    }
    newline();
    output << "return-type-slot: " << func->returnTypeSlot;

    if((func->funcType == VM_Func || func->funcType == VM_CInit) && func->bytecodes != nullptr ) {
        newline();
        output << "bytecodes:";
        DEBUG_BLOCK_START
        for(auto i = 0 ; i < func->bytecodes->size; i ++) {
            if(i > 0){
                newline();
            }
            auto bytecode = &((Bytecode*)(func->bytecodes->bytecodes))[i];
            output << "- " << i << ": " << debugPrint(bytecode, types);
        }
        DEBUG_BLOCK_END
    }
}

std::string debugPrint(Bytecode *bytecode, const std::vector<Type*>& types) {

    static std::unordered_map<Opcode, std::string> maps = {
            { OP_NOP, "nop" },
            { OP_NEW, "new" },
            { OP_OCONST_NIL, "const.nil" },
            { OP_ICONST, "const.int" },
            { OP_SCONST, "const.string" },

            { OP_ISTORE, "store.int" },
            { OP_ILOAD, "load.int" },
            { OP_OLOAD, "oload" },
            { OP_PUTFIELD, "put.field"},
            { OP_GETFIELD, "get.field"},
            { OP_PUTSTATIC, "put.static"},
            { OP_GETSTATIC, "get.static"},

            { OP_IAND, "and.int" },
            { OP_INEG, "neg.int" },
            { OP_ICMP_G, "cmp.large.int" },
            { OP_ICMP_GE, "cmp.large.equals.int" },
            { OP_ICMP_L, "cmp.less.int" },
            { OP_ICMP_LE, "cmp.less.equals.int" },
            { OP_ICMP_NE, "cmp.not.equals.int"},
            { OP_ICMP_EQ, "cmp.equals.int" },
            { OP_IADD, "add.int" },
            { OP_ISUB, "sub.int" },
            { OP_IMUL, "mul.int" },
            { OP_IDIV, "div.int" },
            { OP_IREM, "rem.int" },

            { OP_IFEQ, "if.eq" },
            { OP_IFNE, "if.ne" },
            { OP_IFLT, "if.lt" },
            { OP_IFLE, "if.le" },
            { OP_IFGT, "if.gt" },
            { OP_IFGE, "if.ge" },

            { OP_RETURN, "return" },
            { OP_IRETURN, "return.int" },
            { OP_ORETURN, "return.object" },
            { OP_INVOKE, "invoke" },
            { OP_GOTO, "goto"},
            { OP_DEBUG, "debug"},
            { OP_ONEWARRAY, "new.array.object"},
            { OP_DUP, "dup" },
            { OP_POP, "pop"}
    };


    std::stringstream ss;
    auto instStr = maps[(Opcode)bytecode->format1.op];
    assert(!instStr.empty());
    ss << maps[(Opcode)bytecode->format1.op] ;
    switch((Opcode)bytecode->format1.op) {
        case OP_NEW: {
            auto classTypeSlot = VALUE_FROM_BYTECODE(*bytecode);
            auto klass = (Class*)types[classTypeSlot];
            ss << " " << classTypeSlot << " #class: \"" << klass->name << "\"";
        }
            break;
        case OP_INVOKE: {
            auto methodSlot = VALUE_FROM_BYTECODE(*bytecode);
            auto method = (Function*)types[methodSlot];
            ss << " " << methodSlot << " #method: \"" << method->name << "\"";
        }
            break;
        case OP_GETSTATIC:
        case OP_PUTSTATIC: {
            auto classSlotId = VAL1_FROM_BYTECODE(*bytecode);    // slot id of class
            auto klass = (Class*)types[classSlotId];
            auto fieldOffset = VAL2_FROM_BYTECODE(*bytecode);
            auto field = klass->staticFields[fieldOffset];
            ss << " " << classSlotId << " " << fieldOffset << " #class: \"" << klass->name << "\", field: \"" << field->name << "\""  ;
        }
            break;

        default:
            ss << " " << VALUE_FROM_BYTECODE(*bytecode);
            break;
    }
    return ss.str();

}