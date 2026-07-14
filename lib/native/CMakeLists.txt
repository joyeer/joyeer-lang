add_joyeer_library(JoyeerNativeRuntime STATIC
        runtime.c
        entry.c
)

set_target_properties(JoyeerNativeRuntime PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
)
