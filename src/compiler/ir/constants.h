#ifndef __joyeer_compiler_ir_constants_h__
#define __joyeer_compiler_ir_constants_h__

#include "compiler/frontend/typechecking.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace joyeer::lowering {

// Largest encoded payload of one array constant, in bytes.
inline constexpr size_t maximumConstantBytes = 1024 * 1024;

// A compile-time value. Integers and Booleans keep their two's-complement bits
// masked to the type width; signedness comes from the Joyeer type.
struct ConstantValue {
    enum class Kind { integer, floating, boolean, string, array };

    Kind kind = Kind::integer;
    uint64_t integer = 0;
    double floating = 0;
    std::string text;
    std::vector<ConstantValue> elements;
};

// Evaluates the constant-expression subset of Joyeer on the typed AST:
// literals, array literals, prefix and binary operators, `as` / `&as`, and
// references to other constants. Every failure, including arithmetic overflow,
// is reported through the callback instead of becoming a run-time trap.
class ConstantEvaluator {
public:
    using Report = std::function<void(SourceSpan, std::string)>;
    // Maps a referenced symbol to the initializer of the top-level constant it names.
    using Lookup = std::function<syntax::ExprPtr(semantic::SymbolId)>;

    ConstantEvaluator(const typing::TypeCheckedModel::Ptr& model, Lookup lookup, Report report);

    [[nodiscard]] std::optional<ConstantValue> evaluateSymbol(
            semantic::SymbolId symbol, SourceSpan use);

    // Little-endian payload: scalars at their width, strings and arrays prefixed by
    // a 64-bit length, recursively. Nullopt (after reporting) if over the size limit.
    [[nodiscard]] std::optional<std::string> encode(
            const ConstantValue& value, typing::TypeId type, SourceSpan span);

private:
    [[nodiscard]] std::optional<ConstantValue> evaluate(const syntax::ExprPtr& expression);
    [[nodiscard]] std::optional<ConstantValue> evaluatePrefix(
            const syntax::PrefixExprSyntax::Ptr& expression);
    [[nodiscard]] std::optional<ConstantValue> evaluateBinary(
            const syntax::BinaryExprSyntax::Ptr& expression);
    [[nodiscard]] std::optional<ConstantValue> evaluateCount(
            const syntax::MemberExprSyntax::Ptr& expression);
    [[nodiscard]] std::optional<ConstantValue> evaluateLiteral(
            const syntax::LiteralExprSyntax::Ptr& expression);
    bool appendEncoded(const ConstantValue& value, typing::TypeId type, std::string& out,
            SourceSpan span);
    std::nullopt_t fail(SourceSpan span, std::string message);

    typing::TypeCheckedModel::Ptr model;
    Lookup lookup;
    Report report;
    std::unordered_map<semantic::SymbolId, ConstantValue> cache;
    std::unordered_set<semantic::SymbolId> inProgress;
    std::unordered_set<semantic::SymbolId> failed;
};

} // namespace joyeer::lowering

#endif
