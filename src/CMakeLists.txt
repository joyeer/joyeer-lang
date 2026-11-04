add_subdirectory(runtime)
add_subdirectory(backend)
add_subdirectory(compiler)

# Object generation needs the compiler before a mixed-language runtime exists.
# Stage the usable product separately, rather than making joyeer wait for it.
add_custom_target(JoyeerToolchain ALL
	COMMAND "${CMAKE_COMMAND}" -E copy_if_different
		"$<TARGET_FILE:JoyeerRuntime>"
		"$<TARGET_FILE_DIR:joyeer>/$<TARGET_FILE_NAME:JoyeerRuntime>"
	DEPENDS joyeer JoyeerRuntime
	COMMENT "Staging the Joyeer compiler/runtime product"
	VERBATIM)

add_subdirectory(library)
