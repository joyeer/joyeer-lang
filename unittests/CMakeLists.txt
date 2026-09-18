include(FetchContent)

FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.15.2
)

set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(BUILD_GTEST ON CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(googletest)

include(GoogleTest)

function(joyeer_discover_tests target)
        gtest_discover_tests(${target}
                        DISCOVERY_MODE PRE_TEST
                        DISCOVERY_TIMEOUT 30
                        ${ARGN}
        )
endfunction()

add_subdirectory(runtime)
add_subdirectory(compiler)
# add_subdirectory(vm)
