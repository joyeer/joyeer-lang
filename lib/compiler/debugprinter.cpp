#include "debugprinter.h"
#include <fstream>
#include "joyeer/compiler/compiler+service.h"

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