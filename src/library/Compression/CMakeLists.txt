include(FetchContent)
FetchContent_Declare(joyeer_miniz
    GIT_REPOSITORY https://github.com/richgel999/miniz.git
    GIT_TAG 174573d60290f447c13a2b1b3405de2b96e27d6c # 3.1.0
    SOURCE_SUBDIR joyeer-no-automatic-add-subdirectory)
FetchContent_MakeAvailable(joyeer_miniz)

# Keep the dependency private and in one optional archive, not JoyeerRuntime.
add_library(JoyeerCompression STATIC
    native.c
    "${joyeer_miniz_SOURCE_DIR}/miniz.c"
    "${joyeer_miniz_SOURCE_DIR}/miniz_tdef.c"
    "${joyeer_miniz_SOURCE_DIR}/miniz_tinfl.c"
    "${joyeer_miniz_SOURCE_DIR}/miniz_zip.c")
include(GenerateExportHeader)
generate_export_header(JoyeerCompression BASE_NAME miniz
    EXPORT_FILE_NAME "${CMAKE_CURRENT_BINARY_DIR}/miniz_export.h")
target_include_directories(JoyeerCompression PRIVATE
    "${JOYEER_SRC_DIR}" "${joyeer_miniz_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_definitions(JoyeerCompression PRIVATE
    MINIZ_STATIC_DEFINE MINIZ_NO_STDIO MINIZ_NO_TIME MINIZ_NO_ZLIB_COMPATIBLE_NAMES)
set(package "${CMAKE_CURRENT_BINARY_DIR}/package/$<CONFIG>")
set_target_properties(JoyeerCompression PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON MSVC_RUNTIME_LIBRARY MultiThreaded
    ARCHIVE_OUTPUT_DIRECTORY "${package}/Compression/native"
    JOYEER_PACKAGE_DIRECTORY "${package}")
file(GENERATE OUTPUT "${package}/Compression/compression.joyeer"
    INPUT "${CMAKE_CURRENT_SOURCE_DIR}/compression.joyeer")
file(GENERATE OUTPUT "${package}/Compression/licenses/miniz-LICENSE"
    INPUT "${joyeer_miniz_SOURCE_DIR}/LICENSE")
file(GENERATE OUTPUT "${package}/Compression/licenses/Joyeer-LICENSE"
    INPUT "${JOYEER_SOURCE_DIR}/LICENSE")
file(GENERATE OUTPUT "${package}/Compression/project.toml"
    CONTENT "schema-version = 1\n[module]\nname = 'Compression'\nkind = 'source'\nsources = ['compression.joyeer']\ndependencies = []\nnative-bindings = true\nnative-libraries = ['native/$<TARGET_FILE_NAME:JoyeerCompression>']\n")
file(GENERATE OUTPUT "${package}/CompressionTests/main.joyeer"
    INPUT "${JOYEER_TESTS_DIR}/library/Compression/main.joyeer")
file(GENERATE OUTPUT "${package}/CompressionTests/project.toml"
    CONTENT "schema-version = 1\n[module]\nname = 'CompressionTests'\nkind = 'test'\noutput = 'compression-tests'\nsources = ['main.joyeer']\ndependencies = ['Compression']\n")
file(GENERATE OUTPUT "${package}/workspace.toml"
    CONTENT "schema-version = 1\n[workspace]\nmembers = ['Compression', 'CompressionTests']\n")
install(TARGETS JoyeerCompression ARCHIVE DESTINATION library/Compression/native COMPONENT Compression)
install(FILES "${package}/Compression/project.toml" compression.joyeer
    DESTINATION library/Compression COMPONENT Compression)
install(FILES "${joyeer_miniz_SOURCE_DIR}/LICENSE"
    DESTINATION library/Compression/licenses RENAME miniz-LICENSE COMPONENT Compression)
install(FILES "${JOYEER_SOURCE_DIR}/LICENSE"
    DESTINATION library/Compression/licenses RENAME Joyeer-LICENSE COMPONENT Compression)
