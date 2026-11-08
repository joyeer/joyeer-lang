# The runtime is Joyeer (docs/plan/runtime-in-joyeer.md): two "core" modules compiled
# by the just-built compiler into the two objects the archive holds, the core
# (everything but the entry) and the entry (the C `main` / `wmain` and the program
# startup). They are separate objects so that linking the core into a test
# executable does not add a second entry point to it. runtime.h and native.h stay
# as the C declarations of its ABI for native libraries.
#
# The Joyeer sources are listed explicitly; add a file here when adding it.
set(JOYEER_RUNTIME_CORE_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/joyeer/utf8.joyeer"
        "${CMAKE_CURRENT_SOURCE_DIR}/joyeer/strings.joyeer"
        "${CMAKE_CURRENT_SOURCE_DIR}/joyeer/panic.joyeer"
        "${CMAKE_CURRENT_SOURCE_DIR}/joyeer/streams.joyeer"
        "${CMAKE_CURRENT_SOURCE_DIR}/joyeer/allocator.joyeer"
        "${CMAKE_CURRENT_SOURCE_DIR}/joyeer/text.joyeer"
        "${CMAKE_CURRENT_SOURCE_DIR}/joyeer/print.joyeer"
        "${CMAKE_CURRENT_SOURCE_DIR}/joyeer/arrays.joyeer"
        "${CMAKE_CURRENT_SOURCE_DIR}/joyeer/dictionaries.joyeer"
        "${CMAKE_CURRENT_SOURCE_DIR}/joyeer/floats.joyeer"
)
# The entry logic is shared with the entry encoding tests, a sibling directory.
set(JOYEER_RUNTIME_ENTRY_LOGIC "${CMAKE_CURRENT_SOURCE_DIR}/joyeer/entry.joyeer" CACHE INTERNAL "")
set(JOYEER_RUNTIME_ENTRY_SOURCES
        "${JOYEER_RUNTIME_ENTRY_LOGIC}"
        "${CMAKE_CURRENT_SOURCE_DIR}/joyeer/entry_main.joyeer"
)

# Compiles Joyeer sources as one runtime core module into OBJECT. Optimized in
# every configuration except Debug, which keeps full debug information so a crash
# inside the runtime shows Joyeer frames.
function(joyeer_runtime_core_object OBJECT MODULE COMMENT)
    add_custom_command(
            OUTPUT "${OBJECT}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>"
            COMMAND "$<TARGET_FILE:joyeer>"
                    --module-name ${MODULE}
                    --runtime-core-module ${MODULE}
                    "$<IF:$<CONFIG:Debug>,-O0,-O2>"
                    "$<IF:$<CONFIG:Debug>,-gfull,-g0>"
                    --emit-runtime-object "${OBJECT}"
                    -- ${ARGN}
            DEPENDS joyeer JoyeerBackend ${ARGN}
            COMMENT "${COMMENT}"
            VERBATIM)
    set_source_files_properties("${OBJECT}" PROPERTIES GENERATED TRUE EXTERNAL_OBJECT TRUE)
endfunction()

set(JOYEER_RUNTIME_CORE_OBJECT
        "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/runtime-core${CMAKE_C_OUTPUT_EXTENSION}")
set(JOYEER_RUNTIME_ENTRY_OBJECT
        "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/runtime-entry${CMAKE_C_OUTPUT_EXTENSION}")
joyeer_runtime_core_object("${JOYEER_RUNTIME_CORE_OBJECT}" runtime.core
        "Compiling the Joyeer runtime core" ${JOYEER_RUNTIME_CORE_SOURCES})
joyeer_runtime_core_object("${JOYEER_RUNTIME_ENTRY_OBJECT}" runtime.entry
        "Compiling the Joyeer runtime entry" ${JOYEER_RUNTIME_ENTRY_SOURCES})

# The archive holds the two Joyeer objects and nothing else: there is no C source
# left, so the target needs its linker language named (tests/unittests/runtime
# checks the members).
add_library(JoyeerRuntime STATIC
        "${JOYEER_RUNTIME_CORE_OBJECT}"
        "${JOYEER_RUNTIME_ENTRY_OBJECT}"
)
target_include_directories(JoyeerRuntime PUBLIC ${JOYEER_SRC_DIR})
# The symbol gates in tests/unittests/runtime read the object paths from here.
set_target_properties(JoyeerRuntime PROPERTIES
        JOYEER_CORE_OBJECT "${JOYEER_RUNTIME_CORE_OBJECT}"
        JOYEER_ENTRY_OBJECT "${JOYEER_RUNTIME_ENTRY_OBJECT}")
set_target_properties(JoyeerRuntime PROPERTIES LINKER_LANGUAGE C)
