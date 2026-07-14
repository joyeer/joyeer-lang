#ifndef __joyeer_diagnostic_diagnostic_h__
#define __joyeer_diagnostic_diagnostic_h__

#include <algorithm>
#include <string>
#include <vector>

enum ErrorLevel {
    none,
    // compiler progress will be continuing
    report,
    // compiler will stop work
    failure
};

struct ErrorMessage {
    ErrorMessage(ErrorLevel level, const char* message, int lineAt, int columnAt);

    ErrorLevel level;
    std::string message;
    int lineAt;
    int columnAt;
};

#define DIAGNOSTICS_ERROR_MESSAGE(name, message) static constexpr const char* name = message;

struct Diagnostics {

    explicit Diagnostics(): errors() {};

    DIAGNOSTICS_ERROR_MESSAGE(errorFileImportShouldAtTopOfSourceFile,           "`import` should be presented at the begin of source file")
    DIAGNOSTICS_ERROR_MESSAGE(errorNoSuchFileOrDirectory,                       "no such file or directory:")

    /**
     * Lex error
     */
    DIAGNOSTICS_ERROR_MESSAGE(errorOctalNumberFormat,                           "Octal number only contains 0,1,2,3,4,5,6,7")
    DIAGNOSTICS_ERROR_MESSAGE(errorIntegerLiteralOverflow,                      "Integer literal overflows when stored into 'Int'")
    DIAGNOSTICS_ERROR_MESSAGE(errorUnterminatedCComment,                        "Unterminated '/*' comment")
    DIAGNOSTICS_ERROR_MESSAGE(errorUnterminatedStringLiteral,                   "Unterminated string literal")
    DIAGNOSTICS_ERROR_MESSAGE(errorInvalidStringEscape,                         "Invalid string escape sequence")
    DIAGNOSTICS_ERROR_MESSAGE(errorUnterminatedByteLiteral,                     "Unterminated byte literal")
    DIAGNOSTICS_ERROR_MESSAGE(errorInvalidByteLiteral,                          "A byte literal must contain exactly one ASCII byte")
    DIAGNOSTICS_ERROR_MESSAGE(errorInvalidByteEscape,                           "Invalid byte escape sequence")
    DIAGNOSTICS_ERROR_MESSAGE(errorUnsupportedNumericLiteral,                   "This numeric literal form is not supported")
    DIAGNOSTICS_ERROR_MESSAGE(errorInvalidNumericSuffix,                        "Invalid suffix on decimal integer literal")
    DIAGNOSTICS_ERROR_MESSAGE(errorInvalidSourceCharacter,                      "Invalid source character '%s'")
    DIAGNOSTICS_ERROR_MESSAGE(errorUnsupportedSyntax,                           "Syntax '%s' is not supported in the JSON-parser MVP")

    /**
     * Syntax errors
     */
    DIAGNOSTICS_ERROR_MESSAGE(errorFuncNameMissed,                              "Function name missing")
    DIAGNOSTICS_ERROR_MESSAGE(errorCannotFindFunction,                          "Cannot find '%s' in scope")
    DIAGNOSTICS_ERROR_MESSAGE(errorCannotFindType,                              "Cannot find type '%s' in scope")
    DIAGNOSTICS_ERROR_MESSAGE(errorVarDeclMissingAnnotation,                    "Type annotation missing in variable declaration")
    DIAGNOSTICS_ERROR_MESSAGE(errorNilRequireContextualType,                    "'nil' requires a contextual type")
    DIAGNOSTICS_ERROR_MESSAGE(errorNilCannotInitializeSpecifiedType,            "'nil' cannot initialize specified type '%s'")
    DIAGNOSTICS_ERROR_MESSAGE(errorInvalidRedeclaration,                        "Invalid redeclaration of %s")
    DIAGNOSTICS_ERROR_MESSAGE(errorOptionalAccessWithoutUnwrapping,             "value of optional type '%s?' must be unwrapped to refer to member '%s' of wrapped base type '%s'")
    DIAGNOSTICS_ERROR_MESSAGE(success,                                          "success!")


    void reportError(ErrorLevel level, const char* error, ...);
    void reportError(ErrorLevel level, int lineAt, int columnAt, const char* error, ...);

    // print the error into consoles
    void printErrors();

    // print the error message
    void printError(ErrorMessage& error);

    [[nodiscard]] bool hasFailure() const {
        return std::any_of(errors.begin(), errors.end(), [](const auto& error) {
            return error.level == ErrorLevel::failure;
        });
    }

    std::vector<ErrorMessage> errors;
    
};

#endif // __joyeer_diagnostic_diagnostic_h__
