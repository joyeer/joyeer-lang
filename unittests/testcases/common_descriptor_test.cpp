#include "joyeer/runtime/descriptor.h"
#include <gtest/gtest.h>


TEST(DescriptorTest, FileModileDescritpor) {
    auto descriptor = ModuleDescriptor(std::string("foo.joyeer"));
    auto rawDescriptor = descriptor.getRawDescriptor();
    EXPECT_EQ(rawDescriptor, "#foo;");

    auto descriptor2 = ModuleDescriptor(std::string("bar/foo.joyeer"));
    auto rawDescriptor2 = descriptor2.getRawDescriptor();
    EXPECT_EQ(rawDescriptor2, "#bar/foo;");
    
}

TEST(DescriptorTest, FileModuleInitializerDescriptor) {
    auto descriptor = std::make_shared<ModuleDescriptor>(std::string("foo.joyeer"));
    
    auto fileModuleInitializerDescriptor = std::make_shared<ModuleInitializerDescriptor>(descriptor);
    auto rawDescriptor = fileModuleInitializerDescriptor->getRawDescriptor();
    EXPECT_EQ(rawDescriptor, "#foo;/&module-initializer();");
}

TEST(DescriptorTest, PrimaryTypeChecker) {
    EXPECT_EQ(BoolDescriptor().getRawDescriptor(), "B");
    EXPECT_EQ(IntDescriptor().getRawDescriptor(), "L");
    EXPECT_EQ(UIntDescriptor().getRawDescriptor(), "l");
    EXPECT_EQ(Int8Descriptor().getRawDescriptor(), "D");
    EXPECT_EQ(UInt8Descriptor().getRawDescriptor(), "d");
    EXPECT_EQ(Int16Descriptor().getRawDescriptor(), "H");
    EXPECT_EQ(UInt16Descriptor().getRawDescriptor(), "h");
    EXPECT_EQ(Int32Descriptor().getRawDescriptor(), "K");
    EXPECT_EQ(UInt32Descriptor().getRawDescriptor(), "k");
    EXPECT_EQ(Int128Descriptor().getRawDescriptor(), "X");
    EXPECT_EQ(UInt128Descriptor().getRawDescriptor(), "x");
    EXPECT_EQ(FloatDescriptor().getRawDescriptor(), "F");
    EXPECT_EQ(DoubleDescriptor().getRawDescriptor(), "O");
    EXPECT_EQ(Float128Descriptor().getRawDescriptor(), "W");
    EXPECT_EQ(StringDescriptor().getRawDescriptor(), "S");
    EXPECT_EQ(VoidDescriptor().getRawDescriptor(), "V");
}

