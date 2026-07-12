#include "joyeer/compiler/sourcefile.h"
#include <iostream>
#include <sstream>
#include <fstream>
#include <utility>

SourceFile::SourceFile(const std::string& workingDirectory, const std::string& path) {
    auto modulePath = path.substr(workingDirectory.size() + 1, path.size() - workingDirectory.size() - 1);
    pathInWorkingDirectory = modulePath;
    location = path;
    open(path);
}

SourceFile::SourceFile(std::string sourceContent):
        content(std::move(sourceContent)),
        pathInWorkingDirectory("<memory>"),
        location("<memory>") {
}

void SourceFile::open(const std::string &path) {
    std::ifstream wif(path);
    std::stringstream wss;
    wss << wif.rdbuf();
    
    content = wss.str();
}
