#include "joyeer/diagnostic/diagnostic.h"
#include <iostream>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>

#define BUF_SIZE 2048

ErrorMessage::ErrorMessage(ErrorLevel level, const char* error, int lineAt, int columnAt):
level(level),
message(error),
lineAt(lineAt),
columnAt(columnAt) {

}

void Diagnostics::reportError(ErrorLevel level, const char* errorFormat, ...) {


    char string[BUF_SIZE];
    va_list args;

    va_start(args, errorFormat);
    vsnprintf(string, BUF_SIZE, errorFormat, args);
    va_end(args);

    ErrorMessage e(level, string, -1, -1);

    errors.push_back(e);
}

void Diagnostics::reportError(ErrorLevel level, int lineAt, int columnAt, const char* errorFormat, ...) {

    char string[BUF_SIZE];
    va_list args;

    va_start(args, errorFormat);
    vsnprintf(string, BUF_SIZE, errorFormat, args);
    va_end(args);

    ErrorMessage e(level, string, lineAt, columnAt);

    errors.push_back(e);
}


void Diagnostics::printErrors() {
    for(auto error: errors) {
        printError(error);
    }
}

void Diagnostics::printError(ErrorMessage &error) {
    std::cout << "SyntaxError(line: " << error.lineAt << "):\n    " << error.message << std::endl;
}