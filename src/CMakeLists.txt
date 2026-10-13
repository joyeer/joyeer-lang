add_subdirectory(runtime)
add_subdirectory(backend)
add_subdirectory(compiler)
add_subdirectory(tools)

# Root-level acceptance tests consume the same explicit bootstrap inputs.
if(JOYEER_BUILD_JOYPM)
	set(JOYEER_JOYPM_SOURCES "${JOYEER_JOYPM_SOURCES}" PARENT_SCOPE)
	set(JOYEER_JOYPM_EXECUTABLE "${JOYEER_JOYPM_EXECUTABLE}" PARENT_SCOPE)
	set(JOYEER_JOYPM_PROJECT_DIRECTORY "${JOYEER_JOYPM_PROJECT_DIRECTORY}" PARENT_SCOPE)
endif()