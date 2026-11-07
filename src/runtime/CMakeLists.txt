# The runtime is a mix while it moves into Joyeer (docs/plan/runtime-in-joyeer.md):
# the C sources below, and the Joyeer "core" module compiled by the just-built
# compiler into one object that is archived beside them. A symbol has one
# provider: when a function moves into joyeer/, its C definition is deleted.
#
# The Joyeer sources are listed explicitly; add a file here when adding it.
set(JOYEER_RUNTIME_CORE_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/joyeer/utf8.joyeer"
        "${CMAKE_CURRENT_SOURCE_DIR}/joyeer/strings.joyeer"
        "${CMAKE_CURRENT_SOURCE_DIR}/joyeer/panic.joyeer"
        "${CMAKE_CURRENT_SOURCE_DIR}/joyeer/allocator.joyeer"
        "${CMAKE_CURRENT_SOURCE_DIR}/joyeer/text.joyeer"
        "${CMAKE_CURRENT_SOURCE_DIR}/joyeer/print.joyeer"
)

# Optimized in every configuration except Debug, which keeps full debug
# information so a crash inside the runtime shows Joyeer frames.
set(JOYEER_RUNTIME_CORE_OBJECT
        "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/runtime-core${CMAKE_C_OUTPUT_EXTENSION}")
add_custom_command(
        OUTPUT "${JOYEER_RUNTIME_CORE_OBJECT}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>"
        COMMAND "$<TARGET_FILE:joyeer>"
                --module-name runtime.core
                --runtime-core-module runtime.core
                "$<IF:$<CONFIG:Debug>,-O0,-O2>"
                "$<IF:$<CONFIG:Debug>,-gfull,-g0>"
                --emit-runtime-object "${JOYEER_RUNTIME_CORE_OBJECT}"
                -- ${JOYEER_RUNTIME_CORE_SOURCES}
        DEPENDS joyeer JoyeerBackend ${JOYEER_RUNTIME_CORE_SOURCES}
        COMMENT "Compiling the Joyeer runtime core"
        VERBATIM)
set_source_files_properties("${JOYEER_RUNTIME_CORE_OBJECT}"
        PROPERTIES GENERATED TRUE EXTERNAL_OBJECT TRUE)

add_library(JoyeerRuntime STATIC
        runtime.c
        entry.c
        "${JOYEER_RUNTIME_CORE_OBJECT}"
)
target_include_directories(JoyeerRuntime PUBLIC ${JOYEER_SRC_DIR})
# The symbol gate in tests/unittests/runtime reads the object path from here.
set_target_properties(JoyeerRuntime PROPERTIES JOYEER_CORE_OBJECT "${JOYEER_RUNTIME_CORE_OBJECT}")

set_target_properties(JoyeerRuntime PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        MSVC_RUNTIME_LIBRARY MultiThreaded
)
