cmake_minimum_required(VERSION 3.20)
include("${CMAKE_CURRENT_LIST_DIR}/helpers.cmake")

# This is intentionally not skipped on Windows: the compiler's UTF-8 CLI fix
# is a prerequisite for claiming the same tool contract on all native hosts.
set(package "${work}/project 雪 🦀")
jp_copy_project(single "${package}")
file(RENAME "${package}/main.joyeer" "${package}/source 雪 🦀.joyeer")
file(READ "${package}/joyeer.toml" manifest)
string(REPLACE "main.joyeer" [=[source \u96EA \U0001F980.joyeer]=] manifest "${manifest}")
file(WRITE "${package}/joyeer.toml" "${manifest}")
jp_call(0 "${outside}" check --compiler "${JOYEER_EXECUTABLE}"
    --manifest-path "${package}/joyeer.toml")
jp_no_outputs("${package}")
jp_forward(run "${package}" unicode)
jp_artifacts("${package}" debug sample 1)
string(REPLACE "kind = \"bin\"" "kind = \"test\"" manifest "${manifest}")
file(WRITE "${package}/joyeer.toml" "${manifest}")
jp_forward(test "${package}" unicode)
jp_artifacts("${package}" debug sample 2)
message(STATUS "joypm Unicode paths/manifest escapes/child argv acceptance passed")