# The runtime is Joyeer (docs/plan/runtime-in-joyeer.md): four "core" modules compiled
# by the just-built compiler into the four objects the archive holds: the core
# (everything but the entry), the entry (the program startup logic and, on Windows, the
# own entry point `joyeer_start`) and the main wrappers (the C `main` / `wmain`). They are
# separate objects so that linking the core into a test executable does not add a second
# entry point to it, and a Windows program that starts at `joyeer_start` leaves the C
# wrappers, and so every reference to the C library, out of the link. The fourth object holds
# the thread, mutex and condition functions (decision 0016): on the links that load archive
# members lazily, a program that never references it never gets its operating-system imports.
# runtime.h and native.h stay as the C declarations of its ABI for native libraries.
#
# The Joyeer sources are listed explicitly; add a file here when adding it.
set(JOYEER_RUNTIME_CORE_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/core/utf8.joyeer"
        "${CMAKE_CURRENT_SOURCE_DIR}/core/strings.joyeer"
        "${CMAKE_CURRENT_SOURCE_DIR}/core/panic.joyeer"
        "${CMAKE_CURRENT_SOURCE_DIR}/core/streams.joyeer"
        "${CMAKE_CURRENT_SOURCE_DIR}/core/allocator.joyeer"
        "${CMAKE_CURRENT_SOURCE_DIR}/core/text.joyeer"
        "${CMAKE_CURRENT_SOURCE_DIR}/core/print.joyeer"
        "${CMAKE_CURRENT_SOURCE_DIR}/core/arrays.joyeer"
        "${CMAKE_CURRENT_SOURCE_DIR}/core/dictionaries.joyeer"
        "${CMAKE_CURRENT_SOURCE_DIR}/core/floats.joyeer"
)
# The entry logic is shared with the entry encoding tests, a sibling directory.
set(JOYEER_RUNTIME_ENTRY_LOGIC "${CMAKE_CURRENT_SOURCE_DIR}/core/entry.joyeer" CACHE INTERNAL "")
set(JOYEER_RUNTIME_ENTRY_SOURCES
        "${JOYEER_RUNTIME_ENTRY_LOGIC}"
        "${CMAKE_CURRENT_SOURCE_DIR}/core/entry_start.joyeer"
)
# The C `main` / `wmain` wrappers are a third object: a Windows program that starts at
# `joyeer_start` is linked without it, and so without any reference to the C library.
set(JOYEER_RUNTIME_MAIN_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/core/entry_main.joyeer"
)

# The thread functions are a fourth object, referenced only by programs that use threads.
set(JOYEER_RUNTIME_THREADS_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/core/threads.joyeer"
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
set(JOYEER_RUNTIME_MAIN_OBJECT
        "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/runtime-main${CMAKE_C_OUTPUT_EXTENSION}")
joyeer_runtime_core_object("${JOYEER_RUNTIME_CORE_OBJECT}" runtime.core
        "Compiling the Joyeer runtime core" ${JOYEER_RUNTIME_CORE_SOURCES})
joyeer_runtime_core_object("${JOYEER_RUNTIME_ENTRY_OBJECT}" runtime.entry
        "Compiling the Joyeer runtime entry" ${JOYEER_RUNTIME_ENTRY_SOURCES})
joyeer_runtime_core_object("${JOYEER_RUNTIME_MAIN_OBJECT}" runtime.main
        "Compiling the Joyeer C entry wrappers" ${JOYEER_RUNTIME_MAIN_SOURCES})
set(JOYEER_RUNTIME_THREADS_OBJECT
        "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/runtime-threads${CMAKE_C_OUTPUT_EXTENSION}")
joyeer_runtime_core_object("${JOYEER_RUNTIME_THREADS_OBJECT}" runtime.threads
        "Compiling the Joyeer runtime threads" ${JOYEER_RUNTIME_THREADS_SOURCES})

# The archive holds the four Joyeer objects and nothing else: there is no C source
# left, so the target needs its linker language named (tests/unittests/runtime
# checks the members).
add_library(JoyeerRuntime STATIC
        "${JOYEER_RUNTIME_CORE_OBJECT}"
        "${JOYEER_RUNTIME_ENTRY_OBJECT}"
        "${JOYEER_RUNTIME_MAIN_OBJECT}"
        "${JOYEER_RUNTIME_THREADS_OBJECT}"
)
target_include_directories(JoyeerRuntime PUBLIC ${JOYEER_RUNTIME_INCLUDE_DIR})
# The symbol gates in tests/unittests/runtime read the object paths from here.
set_target_properties(JoyeerRuntime PROPERTIES
        JOYEER_CORE_OBJECT "${JOYEER_RUNTIME_CORE_OBJECT}"
        JOYEER_ENTRY_OBJECT "${JOYEER_RUNTIME_ENTRY_OBJECT}"
        JOYEER_MAIN_OBJECT "${JOYEER_RUNTIME_MAIN_OBJECT}"
        JOYEER_THREADS_OBJECT "${JOYEER_RUNTIME_THREADS_OBJECT}")
set_target_properties(JoyeerRuntime PROPERTIES LINKER_LANGUAGE C)
