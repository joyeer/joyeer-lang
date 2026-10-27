#include "constants.h"

#include <bit>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>

namespace joyeer::lowering {

namespace {

using Kind = typing::TypeKind;
using Value = ConstantValue;


uint64_t widthMask(unsigned bits) {
    return bits >= 64 ? UINT64_MAX : (uint64_t{1} << bits) - 1;
}

// Sign-extends the low `width` bits.
int64_t signedOf(uint64_t bits, unsigned width) {
    bits &= widthMask(width);
    if (width < 64 && ((bits >> (width - 1)) & 1) != 0) bits |= ~widthMask(width);
    return std::bit_cast<int64_t>(bits);
}

// Whether the integer in `bits` is representable in the target type.
bool fitsConversion(uint64_t bits, unsigned sourceWidth, bool sourceSigned,
        unsigned targetWidth, bool targetSigned) {
    bits &= widthMask(sourceWidth);
    if (sourceSigned) {
        const int64_t value = signedOf(bits, sourceWidth);
        if (targetSigned) {
            return targetWidth >= 64 ||
                    (value >= -(int64_t{1} << (targetWidth - 1)) && value < (int64_t{1} << (targetWidth - 1)));
        }
        return value >= 0 && (targetWidth >= 64 || static_cast<uint64_t>(value) < (uint64_t{1} << targetWidth));
    }
    if (targetSigned) return targetWidth >= 64 ? bits <= static_cast<uint64_t>(INT64_MAX)
                                               : bits < (uint64_t{1} << (targetWidth - 1));
    return targetWidth >= 64 || bits < (uint64_t{1} << targetWidth);
}

Value makeInteger(uint64_t bits, unsigned width) {
    Value result;
    result.kind = Value::Kind::integer;
    result.integer = bits & widthMask(width);
    return result;
}

Value makeBoolean(bool value) {
    Value result;
    result.kind = Value::Kind::boolean;
    result.integer = value ? 1 : 0;
    return result;
}

Value makeFloating(double value) {
    Value result;
    result.kind = Value::Kind::floating;
    result.floating = value;
    return result;
}

void appendInteger(std::string& out, uint64_t value, unsigned bytes) {
    for (unsigned index = 0; index < bytes; ++index) {
        out.push_back(static_cast<char>((value >> (8 * index)) & 0xff));
    }
}

} // namespace

ConstantEvaluator::ConstantEvaluator(
        const typing::TypeCheckedModel::Ptr& model, Lookup lookup, Report report):
        model(model), lookup(std::move(lookup)), report(std::move(report)) {}

std::nullopt_t ConstantEvaluator::fail(SourceSpan span, std::string message) {
    report(span, std::move(message));
    return std::nullopt;
}

std::optional<ConstantValue> ConstantEvaluator::evaluateSymbol(
        semantic::SymbolId symbol, SourceSpan use) {
    if (const auto found = cache.find(symbol); found != cache.end()) return found->second;
    if (failed.contains(symbol)) return std::nullopt;
    if (inProgress.contains(symbol)) {
        return fail(use, "constants refer to each other in a cycle");
    }
    const auto initializer = lookup(symbol);
    if (initializer == nullptr) {
        return fail(use, "only top-level 'let' constants can be used in a constant expression");
    }
    inProgress.insert(symbol);
    auto value = evaluate(initializer);
    inProgress.erase(symbol);
    if (value.has_value()) {
        cache.emplace(symbol, *value);
    } else {
        failed.insert(symbol);
    }
    return value;
}

std::optional<ConstantValue> ConstantEvaluator::evaluate(const syntax::ExprPtr& expression) {
    if (expression == nullptr) return std::nullopt;
    switch (expression->kind) {
        case syntax::Kind::literalExpr:
            return evaluateLiteral(std::static_pointer_cast<syntax::LiteralExprSyntax>(expression));
        case syntax::Kind::parenthesizedExpr:
            return evaluate(
                    std::static_pointer_cast<syntax::ParenthesizedExprSyntax>(expression)->expression);
        case syntax::Kind::prefixExpr:
            return evaluatePrefix(std::static_pointer_cast<syntax::PrefixExprSyntax>(expression));
        case syntax::Kind::binaryExpr:
            return evaluateBinary(std::static_pointer_cast<syntax::BinaryExprSyntax>(expression));
        case syntax::Kind::memberExpr:
            if (const auto count = evaluateCount(
                        std::static_pointer_cast<syntax::MemberExprSyntax>(expression));
                count.has_value()) {
                return count;
            }
            [[fallthrough]];
        case syntax::Kind::nameExpr: {
            const auto symbol = model->referencedSymbol(expression);
            if (!symbol.has_value()) {
                return fail(expression->span, "expression is not a compile-time constant");
            }
            return evaluateSymbol(*symbol, expression->span);
        }
        case syntax::Kind::arrayExpr: {
            const auto array = std::static_pointer_cast<syntax::ArrayExprSyntax>(expression);
            Value result;
            result.kind = Value::Kind::array;
            for (const auto& element : array->elements) {
                auto value = evaluate(element);
                if (!value.has_value()) return std::nullopt;
                result.elements.push_back(std::move(*value));
            }
            return result;
        }
        default:
            return fail(expression->span, "expression is not a compile-time constant");
    }
}

// `array.count` and `string.count` (bytes) of a constant operand.
std::optional<ConstantValue> ConstantEvaluator::evaluateCount(
        const syntax::MemberExprSyntax::Ptr& expression) {
    if (expression->member == nullptr || expression->member->rawValue != "count") return std::nullopt;
    const auto baseType = model->typeOf(expression->base);
    if (!baseType.has_value()) return std::nullopt;
    const auto kind = model->types().type(*baseType)->kind;
    if (kind != Kind::array && kind != Kind::string) return std::nullopt;
    const auto base = evaluate(expression->base);
    if (!base.has_value()) return std::nullopt;
    return makeInteger(kind == Kind::array ? base->elements.size() : base->text.size(), 64);
}

std::optional<ConstantValue> ConstantEvaluator::evaluateLiteral(
        const syntax::LiteralExprSyntax::Ptr& expression) {
    const auto type = model->typeOf(expression);
    if (!type.has_value()) return fail(expression->span, "constant has no resolved type");
    const auto kind = model->types().type(*type)->kind;
    const auto& token = expression->literal;
    switch (token->kind) {
        case booleanLiteral:
            return makeBoolean(token->rawValue == "true");
        case stringLiteral: {
            Value result;
            result.kind = Value::Kind::string;
            result.text = token->rawValue;
            return result;
        }
        case byteLiteral:
            return makeInteger(token->intValue, 8);
        case decimalLiteral:
        case floatLiteral: {
            if (typing::isFloating(kind)) {
                const auto& text = token->rawValue;
                double parsed = 0;
                if (kind == Kind::float32) {
                    float narrow = 0;
                    std::from_chars(text.data(), text.data() + text.size(), narrow);
                    parsed = narrow;
                } else {
                    std::from_chars(text.data(), text.data() + text.size(), parsed);
                }
                return makeFloating(parsed);
            }
            if (!typing::isInteger(kind)) break;
            Value result;
            result.kind = Value::Kind::integer;
            result.integer = static_cast<uint64_t>(token->intValue) &
                    widthMask(typing::scalarBitWidth(kind));
            return result;
        }
        default:
            break;
    }
    return fail(expression->span, "literal is not allowed in a constant expression");
}

std::optional<ConstantValue> ConstantEvaluator::evaluatePrefix(
        const syntax::PrefixExprSyntax::Ptr& expression) {
    const auto resultType = model->typeOf(expression);
    if (!resultType.has_value() || expression->op == nullptr) {
        return fail(expression->span, "constant has no resolved type");
    }
    const auto resultKind = model->types().type(*resultType)->kind;
    const auto op = expression->op->kind;

    // `-literal` negates the unsigned magnitude, so the type's minimum is writable.
    if (op == minus && expression->operand->kind == syntax::Kind::literalExpr &&
        typing::isInteger(resultKind)) {
        const auto literal = std::static_pointer_cast<syntax::LiteralExprSyntax>(expression->operand);
        if (literal->literal->kind == decimalLiteral) {
            const auto width = typing::scalarBitWidth(resultKind);
            return makeInteger(uint64_t{0} - static_cast<uint64_t>(literal->literal->intValue), width);
        }
    }

    auto operand = evaluate(expression->operand);
    if (!operand.has_value()) return std::nullopt;

    if (expression->castTarget != nullptr) {
        const auto sourceType = model->typeOf(expression->operand);
        if (!sourceType.has_value()) return fail(expression->span, "constant has no resolved type");
        const auto sourceKind = model->types().type(*sourceType)->kind;
        const auto sourceWidth = typing::scalarBitWidth(sourceKind);
        const auto targetWidth = typing::scalarBitWidth(resultKind);
        const bool targetSigned = typing::isSignedInteger(resultKind);
        if (expression->op->rawValue != "&as" &&
            !fitsConversion(operand->integer, sourceWidth, typing::isSignedInteger(sourceKind),
                    targetWidth, targetSigned)) {
            return fail(expression->span, "integer conversion out of range in constant expression");
        }
        // Widening extends by the source signedness; narrowing keeps the low bits.
        const uint64_t extended = typing::isSignedInteger(sourceKind)
                ? std::bit_cast<uint64_t>(signedOf(operand->integer, sourceWidth))
                : operand->integer;
        return makeInteger(extended, targetWidth);
    }

    switch (op) {
        case bang:
            return makeBoolean(operand->integer == 0);
        case tilde:
            return makeInteger(~operand->integer, typing::scalarBitWidth(resultKind));
        case minus:
            if (typing::isFloating(resultKind)) {
                return makeFloating(-operand->floating);
            } else {
                const auto width = typing::scalarBitWidth(resultKind);
                const int64_t value = signedOf(operand->integer, width);
                const bool isMinimum = width == 64 ? value == std::numeric_limits<int64_t>::min()
                                                   : value == -(int64_t{1} << (width - 1));
                if (isMinimum) {
                    return fail(expression->span, "integer overflow in constant expression");
                }
                return makeInteger(std::bit_cast<uint64_t>(-value), width);
            }
        default:
            return fail(expression->span, "operator is not allowed in a constant expression");
    }
}

std::optional<ConstantValue> ConstantEvaluator::evaluateBinary(
        const syntax::BinaryExprSyntax::Ptr& expression) {
    if (expression->op == nullptr) return fail(expression->span, "constant has no operator");
    const auto leftType = model->typeOf(expression->left);
    const auto rightType = model->typeOf(expression->right);
    if (!leftType.has_value() || !rightType.has_value()) {
        return fail(expression->span, "constant has no resolved type");
    }
    auto left = evaluate(expression->left);
    if (!left.has_value()) return std::nullopt;
    auto right = evaluate(expression->right);
    if (!right.has_value()) return std::nullopt;

    const auto kind = model->types().type(*leftType)->kind;
    const auto rightKind = model->types().type(*rightType)->kind;
    const auto op = expression->op->kind;
    const auto span = expression->span;

    if (kind == Kind::boolean) {
        switch (op) {
            case andAnd: return makeBoolean(left->integer != 0 && right->integer != 0);
            case orOr: return makeBoolean(left->integer != 0 || right->integer != 0);
            case equalEqual: return makeBoolean(left->integer == right->integer);
            case notEqual: return makeBoolean(left->integer != right->integer);
            default: return fail(span, "operator is not allowed in a constant expression");
        }
    }

    if (kind == Kind::string) {
        const int order = left->text.compare(right->text);
        switch (op) {
            case plus: {
                Value result;
                result.kind = Value::Kind::string;
                result.text = left->text + right->text;
                return result;
            }
            case equalEqual: return makeBoolean(order == 0);
            case notEqual: return makeBoolean(order != 0);
            case less: return makeBoolean(order < 0);
            case lessEqual: return makeBoolean(order <= 0);
            case greater: return makeBoolean(order > 0);
            case greaterEqual: return makeBoolean(order >= 0);
            default: return fail(span, "operator is not allowed in a constant expression");
        }
    }

    if (typing::isFloating(kind)) {
        const auto narrow = [kind](double value) {
            return kind == Kind::float32 ? static_cast<double>(static_cast<float>(value)) : value;
        };
        const double a = left->floating;
        const double b = right->floating;
        double result = 0;
        switch (op) {
            case plus: result = narrow(a + b); break;
            case minus: result = narrow(a - b); break;
            case multiply: result = narrow(a * b); break;
            case divide: result = narrow(a / b); break;
            case equalEqual: return makeBoolean(a == b);
            case notEqual: return makeBoolean(a != b);
            case less: return makeBoolean(a < b);
            case lessEqual: return makeBoolean(a <= b);
            case greater: return makeBoolean(a > b);
            case greaterEqual: return makeBoolean(a >= b);
            default: return fail(span, "operator is not allowed in a constant expression");
        }
        if (!std::isfinite(result)) {
            return fail(span, "floating-point constant is not finite");
        }
        return makeFloating(result);
    }

    if (!typing::isInteger(kind)) {
        return fail(span, "operator is not allowed in a constant expression");
    }
    const auto width = typing::scalarBitWidth(kind);
    const bool isSigned = typing::isSignedInteger(kind);
    const uint64_t ua = left->integer;
    const uint64_t ub = right->integer;
    const int64_t sa = signedOf(ua, width);
    const int64_t sb = signedOf(ub, width);
    const int64_t minimum = width == 64 ? std::numeric_limits<int64_t>::min() : -(int64_t{1} << (width - 1));

    switch (op) {
        case plus:
        case minus:
        case multiply: {
            // Unsigned arithmetic wraps like the language; signed overflow is an error.
            const uint64_t wrapped = op == plus ? ua + ub : op == minus ? ua - ub : ua * ub;
            if (!isSigned) return makeInteger(wrapped, width);
            const int64_t result = signedOf(wrapped, width);
            bool exact = false;
            if (width < 64) {
                const int64_t full = op == plus ? sa + sb : op == minus ? sa - sb : sa * sb;
                exact = fitsConversion(std::bit_cast<uint64_t>(full), 64, true, width, true);
            } else if (op == plus) {
                exact = ((sa ^ result) & (sb ^ result)) >= 0;
            } else if (op == minus) {
                exact = ((sa ^ sb) & (sa ^ result)) >= 0;
            } else {
                exact = sa == 0 || sb == 0 ||
                        (!(sa == -1 && sb == minimum) && !(sb == -1 && sa == minimum) &&
                         result / sb == sa);
            }
            if (!exact) return fail(span, "integer overflow in constant expression");
            return makeInteger(wrapped, width);
        }
        case divide:
        case percentage: {
            if (ub == 0) {
                return fail(span, op == divide ? "integer division by zero in constant expression"
                                               : "integer remainder by zero in constant expression");
            }
            if (!isSigned) return makeInteger(op == divide ? ua / ub : ua % ub, width);
            if (sa == minimum && sb == -1) {
                return fail(span, op == divide ? "integer division overflow in constant expression"
                                               : "integer remainder overflow in constant expression");
            }
            return makeInteger(std::bit_cast<uint64_t>(op == divide ? sa / sb : sa % sb), width);
        }
        case ampersand: return makeInteger(ua & ub, width);
        case pipe: return makeInteger(ua | ub, width);
        case caret: return makeInteger(ua ^ ub, width);
        case shiftLeft:
        case shiftRight: {
            const auto countWidth = typing::scalarBitWidth(rightKind);
            const bool negative = typing::isSignedInteger(rightKind) && signedOf(ub, countWidth) < 0;
            const uint64_t count = ub & widthMask(countWidth);
            if (negative || count >= width) {
                return fail(span, "integer shift count out of range in constant expression");
            }
            if (op == shiftLeft) return makeInteger(ua << count, width);
            if (isSigned) return makeInteger(std::bit_cast<uint64_t>(sa >> count), width);
            return makeInteger(ua >> count, width);
        }
        case equalEqual: return makeBoolean(ua == ub);
        case notEqual: return makeBoolean(ua != ub);
        case less: return makeBoolean(isSigned ? sa < sb : ua < ub);
        case lessEqual: return makeBoolean(isSigned ? sa <= sb : ua <= ub);
        case greater: return makeBoolean(isSigned ? sa > sb : ua > ub);
        case greaterEqual: return makeBoolean(isSigned ? sa >= sb : ua >= ub);
        default:
            return fail(span, "operator is not allowed in a constant expression");
    }
}

std::optional<std::string> ConstantEvaluator::encode(
        const ConstantValue& value, typing::TypeId type, SourceSpan span) {
    std::string out;
    if (!appendEncoded(value, type, out, span)) return std::nullopt;
    return out;
}

bool ConstantEvaluator::appendEncoded(const ConstantValue& value, typing::TypeId type,
        std::string& out, SourceSpan span) {
    const auto* record = model->types().type(type);
    if (record == nullptr) {
        report(span, "constant has no resolved type");
        return false;
    }
    switch (record->kind) {
        case Kind::boolean:
            appendInteger(out, value.integer, 1);
            break;
        case Kind::float32: {
            const float narrow = static_cast<float>(value.floating);
            uint32_t bits = 0;
            std::memcpy(&bits, &narrow, sizeof(bits));
            appendInteger(out, bits, 4);
            break;
        }
        case Kind::float64: {
            uint64_t bits = 0;
            std::memcpy(&bits, &value.floating, sizeof(bits));
            appendInteger(out, bits, 8);
            break;
        }
        case Kind::string:
            appendInteger(out, value.text.size(), 8);
            out += value.text;
            break;
        case Kind::array:
            appendInteger(out, value.elements.size(), 8);
            for (const auto& element : value.elements) {
                if (!appendEncoded(element, record->arguments.at(0), out, span)) return false;
                if (out.size() > maximumConstantBytes) break;
            }
            break;
        default:
            if (!typing::isInteger(record->kind)) {
                report(span, "constant type is not supported");
                return false;
            }
            appendInteger(out, value.integer, typing::scalarBitWidth(record->kind) / 8);
            break;
    }
    if (out.size() > maximumConstantBytes) {
        report(span, "constant data exceeds the 1 MiB limit");
        return false;
    }
    return true;
}

} // namespace joyeer::lowering
