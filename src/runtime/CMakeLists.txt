add_library(JoyeerRuntime STATIC
        runtime.c
        entry.c
        host.c
        filesystem.c
        process.c
)
target_include_directories(JoyeerRuntime PUBLIC ${JOYEER_SRC_DIR})

set_target_properties(JoyeerRuntime PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        MSVC_RUNTIME_LIBRARY MultiThreaded
)

if(MSVC)
    target_compile_options(JoyeerRuntime PRIVATE
            $<$<COMPILE_LANGUAGE:C>:/experimental:c11atomics>
    )
endif()
