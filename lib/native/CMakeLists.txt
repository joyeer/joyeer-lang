add_joyeer_library(JoyeerNativeRuntime STATIC
        runtime.c
        entry.c
)

set_target_properties(JoyeerNativeRuntime PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

if(MSVC)
    target_compile_options(JoyeerNativeRuntime PRIVATE
            $<$<COMPILE_LANGUAGE:C>:/experimental:c11atomics>
    )
endif()
