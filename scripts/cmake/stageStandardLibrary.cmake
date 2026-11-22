# Stage the standard modules and lib/index.toml beside the compiler.
#
# Required: DESTINATION (the toolchain's lib directory), LIBRARY_SOURCE
# (lib), VERSION, TARGET, CONFIG. Optional: COMPRESSION_PACKAGE (the
# generated Compression module directory, including its native archive).
foreach(name IN ITEMS DESTINATION LIBRARY_SOURCE VERSION TARGET CONFIG)
    if(NOT DEFINED ${name})
        message(FATAL_ERROR "stageStandardLibrary.cmake: ${name} is required")
    endif()
endforeach()

# Remove stale module sources without touching the runtime archive staged in lib/.
file(REMOVE_RECURSE "${DESTINATION}/Foundation" "${DESTINATION}/System" "${DESTINATION}/Compression")
file(MAKE_DIRECTORY "${DESTINATION}")

set(modules "")
foreach(module IN ITEMS Foundation System)
    file(GLOB files RELATIVE "${LIBRARY_SOURCE}/${module}" "${LIBRARY_SOURCE}/${module}/*")
    foreach(file IN LISTS files)
        file(COPY "${LIBRARY_SOURCE}/${module}/${file}" DESTINATION "${DESTINATION}/${module}")
    endforeach()
    list(APPEND modules "${module}")
endforeach()

if(DEFINED COMPRESSION_PACKAGE AND NOT COMPRESSION_PACKAGE STREQUAL "")
    file(COPY "${COMPRESSION_PACKAGE}/" DESTINATION "${DESTINATION}/Compression")
    list(APPEND modules Compression)
endif()

list(TRANSFORM modules PREPEND "\"")
list(TRANSFORM modules APPEND "\"")
list(JOIN modules ", " module_list)
string(TOLOWER "${CONFIG}" configuration)
file(WRITE "${DESTINATION}/index.toml"
    "schema-version = 1\n"
    "compiler-version = \"${VERSION}\"\n"
    "target = \"${TARGET}\"\n"
    "configuration = \"${configuration}\"\n"
    "modules = [${module_list}]\n")
