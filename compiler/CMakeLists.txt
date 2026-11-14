add_subdirectory(backend)

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${JOYEER_SOURCE_DIR}/lib/Runtime/concurrency.joyeer")
file(READ "${JOYEER_SOURCE_DIR}/lib/Runtime/concurrency.joyeer" JOYEER_PRELUDE_SOURCE)
configure_file(frontend/prelude_source.h.in generated/prelude_source.h @ONLY)

# One private object target serves the compiler and its stage-specific tests.
# Source directories describe responsibilities, not separately shipped libraries.
add_library(JoyeerCompilerCore OBJECT
	codegen/llvm.cpp
	codegen/llvm.h
	codegen/linker.cpp
	codegen/linker.h
	diagnostics/diagnostic.cpp
	diagnostics/diagnostic.h
	driver/arguments.cpp
	driver/arguments.h
	driver/compiler+service.cpp
	driver/compiler+service.h
	driver/driver.cpp
	driver/driver.h
	driver/options.h
	driver/toolchain.cpp
	driver/toolchain.h
	frontend/hostbuiltins.h
	frontend/nativebinding.h
	frontend/lexparser.cpp
	frontend/lexparser.h
	frontend/nameresolution.cpp
	frontend/nameresolution.h
	frontend/parser.cpp
	frontend/parser.h
	frontend/platform.cpp
	frontend/platform.h
	frontend/prelude.cpp
	frontend/prelude.h
	frontend/semantic.cpp
	frontend/semantic.h
	frontend/semanticanalysis.cpp
	frontend/semanticanalysis.h
	frontend/sourcefile.cpp
	frontend/sourcefile.h
	frontend/syntax.cpp
	frontend/syntax.h
	frontend/token.cpp
	frontend/token.h
	frontend/typechecking.cpp
	frontend/typechecking.h
	ir/constants.cpp
	ir/constants.h
	ir/ir.cpp
	ir/ir.h
	ir/irlowering.cpp
	ir/irlowering.h
)
target_include_directories(JoyeerCompilerCore PUBLIC ${JOYEER_SOURCE_DIR} ${JOYEER_RUNTIME_INCLUDE_DIR})
target_include_directories(JoyeerCompilerCore PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(JoyeerCompilerCore PRIVATE JoyeerBackend)
target_compile_definitions(JoyeerCompilerCore PRIVATE
	JOYEER_RUNTIME_PATH="$<TARGET_FILE_NAME:JoyeerRuntime>"
	JOYEER_TOOLCHAIN_VERSION="${JOYEER_TOOLCHAIN_VERSION}"
	JOYEER_TOOLCHAIN_TARGET="${JOYEER_TOOLCHAIN_TARGET}"
	JOYEER_TOOLCHAIN_CONFIG="$<LOWER_CASE:$<CONFIG>>"
	JOYEER_SDK_ROOT_PATH="${JOYEER_MACOS_SDK_PATH}"
	JOYEER_SDK_VERSION="${JOYEER_MACOS_SDK_VERSION}"
	JOYEER_MACOS_DEPLOYMENT_TARGET="${JOYEER_MACOS_DEPLOYMENT_TARGET}"
	JOYEER_DSYMUTIL_EXECUTABLE_PATH="${JOYEER_DSYMUTIL_EXECUTABLE}"
)

add_executable(joyeer main.cpp)
target_link_libraries(joyeer PRIVATE JoyeerCompilerCore)
if(APPLE)
	set_target_properties(joyeer PROPERTIES INSTALL_RPATH "@loader_path")
elseif(UNIX)
	set_target_properties(joyeer PROPERTIES INSTALL_RPATH "$ORIGIN")
endif()