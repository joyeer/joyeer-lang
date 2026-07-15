#include "joyeer/diagnostic/diagnostic.h"
#include <iostream>
#include <cstdio>
#include <cstdarg>
#include <algorithm>

#define BUF_SIZE 2048

ErrorMessage::ErrorMessage(ErrorLevel level, const char* error, int lineAt, int columnAt):
level(level),
message(error),
lineAt(lineAt),
columnAt(columnAt) {

}

void Diagnostics::reportError(ErrorLevel level, const char* errorFormat, ...) {


    char string[BUF_SIZE];
    memset(string, '\0',  BUF_SIZE);
    va_list args;

    va_start(args, errorFormat);
    vsnprintf(string, BUF_SIZE, errorFormat, args);
    va_end(args);

    ErrorMessage e(level, string, -1, -1);

    errors.push_back(e);
}

void Diagnostics::reportError(ErrorLevel level, int lineAt, int columnAt, const char* errorFormat, ...) {

    char string[BUF_SIZE];
    memset(string, '\0',  BUF_SIZE);
    va_list args;

    va_start(args, errorFormat);
    vsnprintf(string, BUF_SIZE, errorFormat, args);
    va_end(args);

    ErrorMessage e(level, string, lineAt, columnAt);

    errors.push_back(e);
}

    void Diagnostics::reportDiagnostic(
        ErrorLevel level,
        std::string code,
        std::string message) {
        ErrorMessage error(level, message.c_str(), -1, -1);
        error.code = std::move(code);
        errors.push_back(std::move(error));
    }

    void Diagnostics::reportSourceDiagnostic(
        ErrorLevel level,
        std::string code,
        std::string path,
        const std::string& source,
        const std::vector<uint32_t>& lineStarts,
        uint32_t offset,
        uint32_t length,
        std::string message,
        std::optional<std::string> help,
        std::optional<DiagnosticFixIt> fixIt) {
        const auto boundedOffset = static_cast<uint32_t>(std::min<size_t>(offset, source.size()));
        const auto upper = std::upper_bound(
            lineStarts.begin(),
            lineStarts.end(),
            boundedOffset);
        const auto line = upper == lineStarts.begin()
            ? 0
            : static_cast<int>(std::distance(lineStarts.begin(), upper) - 1);
        const auto lineStart = lineStarts.empty()
            ? 0u
            : lineStarts[static_cast<size_t>(line)];
        auto lineEnd = source.find_first_of("\r\n", lineStart);
        if (lineEnd == std::string::npos) lineEnd = source.size();

        ErrorMessage error(
            level,
            message.c_str(),
            line,
            static_cast<int>(boundedOffset - lineStart));
        error.code = std::move(code);
        error.path = std::move(path);
        error.sourceLine = source.substr(lineStart, lineEnd - lineStart);
        const auto remaining = lineEnd > boundedOffset ? lineEnd - boundedOffset : size_t { 0 };
        error.length = static_cast<uint32_t>(std::max<size_t>(
            1,
            std::min<size_t>(length == 0 ? 1 : length, remaining)));
        error.hasSourceContext = true;
            error.help = std::move(help);
            error.fixIt = std::move(fixIt);
            if (error.fixIt.has_value()) {
            const auto fixOffset = static_cast<uint32_t>(std::min<size_t>(
                error.fixIt->offset,
                source.size()));
            const auto fixUpper = std::upper_bound(
                lineStarts.begin(),
                lineStarts.end(),
                fixOffset);
            error.fixLineAt = fixUpper == lineStarts.begin()
                ? 0
                : static_cast<int>(std::distance(lineStarts.begin(), fixUpper) - 1);
            const auto fixLineStart = lineStarts.empty()
                ? 0u
                : lineStarts[static_cast<size_t>(error.fixLineAt)];
            error.fixColumnAt = static_cast<int>(fixOffset - fixLineStart);
            }
        errors.push_back(std::move(error));
    }


void Diagnostics::printErrors() {
    for(auto error: errors) {
        printError(error);
    }
}

void Diagnostics::printError(ErrorMessage &error) {
    if (error.hasSourceContext) {
        const auto* severity = error.level == ErrorLevel::report ? "warning" : "error";
        std::cout << error.path << ':' << error.lineAt + 1 << ':' << error.columnAt + 1
                  << ": " << severity;
        if (!error.code.empty()) std::cout << '[' << error.code << ']';
        std::cout << ": " << error.message << '\n';

        std::string expanded;
        size_t visualColumn = 0;
        for (size_t index = 0; index < error.sourceLine.size(); ++index) {
            if (error.sourceLine[index] == '\t') {
                const auto spaces = 4 - (expanded.size() % 4);
                expanded.append(spaces, ' ');
                if (index < static_cast<size_t>(error.columnAt)) visualColumn += spaces;
            } else {
                expanded.push_back(error.sourceLine[index]);
                if (index < static_cast<size_t>(error.columnAt)) ++visualColumn;
            }
        }
        const auto lineText = std::to_string(error.lineAt + 1);
        std::cout << "  " << lineText << " | " << expanded << '\n'
                  << std::string(lineText.size() + 3, ' ') << "| "
                  << std::string(visualColumn, ' ') << '^';
        if (error.length > 1) std::cout << std::string(error.length - 1, '~');
        std::cout << '\n';
        if (error.help.has_value()) {
            std::cout << "help: " << *error.help << '\n';
        }
        if (error.fixIt.has_value()) {
            std::cout << "fix-it: " << error.path << ':'
                      << error.fixLineAt + 1 << ':' << error.fixColumnAt + 1
                      << ':' << error.fixIt->length << ": \"";
            for (const auto character : error.fixIt->replacement) {
                if (character == '\\' || character == '"') std::cout << '\\';
                if (character == '\n') std::cout << "\\n";
                else if (character == '\r') std::cout << "\\r";
                else if (character == '\t') std::cout << "\\t";
                else std::cout << character;
            }
            std::cout << '"' << '\n';
        }
        std::cout.flush();
        return;
    }
    if (!error.code.empty()) {
        const auto* severity = error.level == ErrorLevel::report ? "warning" : "error";
        std::cout << severity << '[' << error.code << "]: "
                  << error.message << std::endl;
        return;
    }
    const auto* label = error.level == ErrorLevel::report ? "Warning" : "SyntaxError";
    std::cout << label << "(line: " << error.lineAt << "):\n    " << error.message << std::endl;
}