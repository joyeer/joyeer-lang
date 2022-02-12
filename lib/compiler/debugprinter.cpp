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


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

NodeDebugPrinter::NodeDebugPrinter(const std::string& filename):
    YMLPrinter(filename) {
}

