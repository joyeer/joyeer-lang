#include "joyeer/compiler/nameresolution.h"
#include "joyeer/compiler/typechecking.h"

#include <gtest/gtest.h>

#include <memory>
#include <vector>

namespace {

using joyeer::typing::TypeContext;
using joyeer::typing::TypeKind;

class TypeContextTest : public testing::Test {
protected:
    void SetUp() override {
        const auto root = std::make_shared<joyeer::syntax::SourceFileSyntax>(
                SourceSpan {},
                std::vector<joyeer::syntax::NodePtr> {});
        resolution = joyeer::semantic::NameResolver().resolve(root);
        ASSERT_TRUE(resolution.succeeded());
        ASSERT_NE(resolution.model, nullptr);
        types = std::make_unique<TypeContext>(*resolution.model);
    }

    const joyeer::typing::TypeRecord& type(joyeer::typing::TypeId id) const {
        const auto* result = types->type(id);
        EXPECT_NE(result, nullptr);
        return *result;
    }

    joyeer::semantic::NameResolutionResult resolution;
    std::unique_ptr<TypeContext> types;
};

TEST_F(TypeContextTest, RegistersConcreteBuiltinTypes) {
    EXPECT_EQ(type(types->voidType()).kind, TypeKind::voidType);
    EXPECT_EQ(type(types->neverType()).kind, TypeKind::never);
    EXPECT_EQ(type(types->anyType()).kind, TypeKind::any);
    EXPECT_EQ(type(types->intType()).kind, TypeKind::integer);
    EXPECT_EQ(type(types->boolType()).kind, TypeKind::boolean);
    EXPECT_EQ(type(types->stringType()).kind, TypeKind::string);
    EXPECT_EQ(type(types->uint8Type()).kind, TypeKind::uint8);

    const auto intSymbol = types->builtinSymbol("Int");
    ASSERT_TRUE(intSymbol.has_value());
    EXPECT_EQ(types->typeForSymbol(*intSymbol), types->intType());
    EXPECT_EQ(types->displayName(types->intType()), "Int");
}

TEST_F(TypeContextTest, InternsConcreteBuiltinGenericApplications) {
    const auto array = types->arrayType(types->intType());
    const auto optional = types->optionalType(types->stringType());
    const auto dictionary = types->dictionaryType(types->stringType(), array);
    const auto result = types->resultType(dictionary, optional);

    EXPECT_EQ(types->arrayType(types->intType()), array);
    EXPECT_EQ(types->optionalType(types->stringType()), optional);
    EXPECT_EQ(type(array).kind, TypeKind::array);
    EXPECT_EQ(type(array).arguments, std::vector { types->intType() });
    EXPECT_EQ(type(dictionary).kind, TypeKind::dictionary);
    EXPECT_EQ(type(result).kind, TypeKind::result);
    EXPECT_EQ(
            types->displayName(result),
            "Result<[String: [Int]], String?>");
}

TEST_F(TypeContextTest, RejectsInvalidTypeConstructorArityAndSymbols) {
    const auto arraySymbol = types->builtinSymbol("Array");
    const auto intSymbol = types->builtinSymbol("Int");
    ASSERT_TRUE(arraySymbol.has_value());
    ASSERT_TRUE(intSymbol.has_value());

    EXPECT_FALSE(types->typeForSymbol(*arraySymbol).has_value());
    EXPECT_FALSE(types->typeForSymbol(
            *arraySymbol,
            { types->intType(), types->stringType() }).has_value());
    EXPECT_FALSE(types->typeForSymbol(
            *intSymbol,
            { types->intType() }).has_value());
    EXPECT_FALSE(types->typeForSymbol(joyeer::semantic::invalidSymbolId).has_value());
}

TEST_F(TypeContextTest, PropagatesErrorTypesWithoutCreatingCompositeNoise) {
    const auto before = types->size();
    EXPECT_EQ(types->arrayType(types->errorType()), types->errorType());
    EXPECT_EQ(types->resultType(types->intType(), types->errorType()), types->errorType());
    EXPECT_EQ(types->size(), before);
    EXPECT_EQ(types->displayName(types->errorType()), "<error>");
}

} // namespace