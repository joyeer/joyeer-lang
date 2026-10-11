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
	frontend/hostbuiltins.h
	frontend/lexparser.cpp
	frontend/lexparser.h
	frontend/nameresolution.cpp
	frontend/nameresolution.h
	frontend/parser.cpp
	frontend/parser.h
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
	ir/ir.cpp
	ir/ir.h
	ir/irlowering.cpp
	ir/irlowering.h
)
target_include_directories(JoyeerCompilerCore PUBLIC ${JOYEER_SRC_DIR})
target_link_libraries(JoyeerCompilerCore PRIVATE JoyeerNativeBackend)
target_compile_definitions(JoyeerCompilerCore PRIVATE
	JOYEER_NATIVE_RUNTIME_PATH="$<TARGET_FILE_NAME:JoyeerNativeRuntime>"
	JOYEER_SDK_ROOT_PATH="${JOYEER_MACOS_SDK_PATH}"
	JOYEER_SDK_VERSION="${JOYEER_MACOS_SDK_VERSION}"
	JOYEER_MACOS_DEPLOYMENT_TARGET="${JOYEER_MACOS_DEPLOYMENT_TARGET}"
	JOYEER_DSYMUTIL_EXECUTABLE_PATH="${JOYEER_DSYMUTIL_EXECUTABLE}"
)

add_executable(joyeer main.cpp)
target_link_libraries(joyeer PRIVATE JoyeerCompilerCore)
add_dependencies(joyeer JoyeerNativeRuntime)
set_property(TARGET joyeer APPEND PROPERTY LINK_DEPENDS
	"$<TARGET_FILE:JoyeerNativeRuntime>"
)
add_custom_command(TARGET joyeer POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
		"$<TARGET_FILE:JoyeerNativeRuntime>"
		"$<TARGET_FILE_DIR:joyeer>/$<TARGET_FILE_NAME:JoyeerNativeRuntime>"
)
if(APPLE)
	set_target_properties(joyeer PROPERTIES INSTALL_RPATH "@loader_path")
elseif(UNIX)
	set_target_properties(joyeer PROPERTIES INSTALL_RPATH "$ORIGIN")
endif()