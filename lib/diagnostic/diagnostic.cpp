#include "joyeer/diagnostic/diagnostic.h"
#include <iostream>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <algorithm>

#define BUF_SIZE 2048

namespace {

struct SourceContext {
    int lineAt = 0;
    int columnAt = 0;
    std::string sourceLine;
    uint32_t length = 1;
};

SourceContext resolveSourceContext(
        const std::string& source,
        const std::vector<uint32_t>& lineStarts,
        uint32_t offset,
        uint32_t length) {
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
    const auto remaining = lineEnd > boundedOffset ? lineEnd - boundedOffset : size_t { 0 };
    return SourceContext {
        line,
        static_cast<int>(boundedOffset - lineStart),
        source.substr(lineStart, lineEnd - lineStart),
        static_cast<uint32_t>(std::max<size_t>(
                1,
                std::min<size_t>(length == 0 ? 1 : length, remaining))),
    };
}

void printSourceExcerpt(
        int lineAt,
        int columnAt,
        const std::string& sourceLine,
        uint32_t length) {
    std::string expanded;
    size_t visualColumn = 0;
    for (size_t index = 0; index < sourceLine.size(); ++index) {
        if (sourceLine[index] == '\t') {
            const auto spaces = 4 - (expanded.size() % 4);
            expanded.append(spaces, ' ');
            if (index < static_cast<size_t>(columnAt)) visualColumn += spaces;
        } else {
            expanded.push_back(sourceLine[index]);
            if (index < static_cast<size_t>(columnAt)) ++visualColumn;
        }
    }
    const auto lineText = std::to_string(lineAt + 1);
    std::cout << "  " << lineText << " | " << expanded << '\n'
              << std::string(lineText.size() + 3, ' ') << "| "
              << std::string(visualColumn, ' ') << '^';
    if (length > 1) std::cout << std::string(length - 1, '~');
    std::cout << '\n';
}

} // namespace

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
        std::optional<DiagnosticFixIt> fixIt,
        std::vector<DiagnosticSourceNote> notes) {
        const auto context = resolveSourceContext(source, lineStarts, offset, length);

        ErrorMessage error(
            level,
            message.c_str(),
            context.lineAt,
            context.columnAt);
        error.code = std::move(code);
        error.path = std::move(path);
        error.sourceLine = context.sourceLine;
        error.length = context.length;
        error.hasSourceContext = true;
        error.help = std::move(help);
        error.fixIt = std::move(fixIt);
        error.notes.reserve(notes.size());
        for (auto& note : notes) {
            const auto noteContext = resolveSourceContext(
                    source,
                    lineStarts,
                    note.offset,
                    note.length);
            error.notes.push_back(ErrorMessageNote {
                std::move(note.message),
                noteContext.lineAt,
                noteContext.columnAt,
                noteContext.sourceLine,
                noteContext.length,
            });
        }
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

        printSourceExcerpt(
                error.lineAt,
                error.columnAt,
                error.sourceLine,
                error.length);
        for (const auto& note : error.notes) {
            std::cout << error.path << ':' << note.lineAt + 1 << ':' << note.columnAt + 1
                      << ": note: " << note.message << '\n';
            printSourceExcerpt(
                    note.lineAt,
                    note.columnAt,
                    note.sourceLine,
                    note.length);
        }
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