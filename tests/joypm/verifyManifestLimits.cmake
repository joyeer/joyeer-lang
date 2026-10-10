cmake_minimum_required(VERSION 3.20)
include("${CMAKE_CURRENT_LIST_DIR}/helpers.cmake")
set(jp_timeout 30)

# No accepted limit case spawns a compiler: source strings need not exist,
# enormous paths need not fit the filesystem, and argv need not fit CreateProcess.
# The driver is compiled in joypm.app and calls parseManifest directly.
function(limit_call name contents expected_output)
    set(package "${work}/logic ${name}")
    set(cwd "${work}/logic caller ${name}")
    file(MAKE_DIRECTORY "${package}" "${cwd}")
    jp_write_bytes("${package}/joyeer.toml" "${contents}")
    execute_process(
        COMMAND "${LOGIC_EXECUTABLE}" "${package}/joyeer.toml" ${ARGN}
        WORKING_DIRECTORY "${cwd}"
        RESULT_VARIABLE status OUTPUT_VARIABLE jp_out ERROR_VARIABLE jp_err
        TIMEOUT 30)
    string(REPLACE "\r\n" "\n" jp_out "${jp_out}")
    jp_no_runtime_error()
    if(NOT status STREQUAL "0" OR NOT jp_err STREQUAL "" OR
            NOT jp_out STREQUAL expected_output)
        jp_fail("Direct manifest limit ${name} failed (${status}); expected:\n${expected_output}")
    endif()
    if(EXISTS "${cwd}/compiler-record.txt")
        jp_fail("Pure manifest limit ${name} unexpectedly spawned a compiler")
    endif()
    jp_no_outputs("${package}")
endfunction()

function(accept_limit name contents modules targets sources version)
    string(LENGTH "${version}" version_bytes)
    set(expected "manifest ok: modules=${modules} targets=${targets} sources=${sources} version=${version_bytes}\n")
    limit_call("${name}" "${contents}" "${expected}" accept "${version}")
endfunction()

function(reject_limit name contents diagnostic pos)
    limit_call("${name}" "${contents}" "manifest rejected at byte ${pos}\n"
        reject "${diagnostic}" "${pos}")
    # Also freeze the CLI diagnostic contract at the same original byte offset.
    # This spy must NOT run: every oversized manifest fails before planning.
    jp_reject_manifest("cli ${name}" "${contents}" "${pos}")
endfunction()

set(header "schema-version = 1\n[package]\nname = 'limits'\nversion = '0.1.0'\n")
set(module "[[modules]]\nname = 'limits.app'\nsources = ['main.joyeer']\ndependencies = []\n")
set(target "[[targets]]\nname = 'limits'\nkind = 'bin'\nmodule = 'limits.app'\n")
set(base "${header}${module}${target}")

# Exactly 65536/65537 ORIGINAL bytes, including multibyte UTF-8 and CRLF.
set(input "# 雪🦀\r\n${base}# ")
string(LENGTH "${input}" input_bytes)
math(EXPR padding_bytes "65536 - ${input_bytes}")
string(REPEAT "x" "${padding_bytes}" padding)
string(APPEND input "${padding}")
string(LENGTH "${input}" input_bytes)
if(NOT input_bytes EQUAL 65536)
    message(FATAL_ERROR "Input-limit fixture is not exactly 65536 bytes")
endif()
accept_limit(input-65536 "${input}" 1 1 1 "0.1.0")
string(APPEND input "x")
string(LENGTH "${input}" input_bytes)
if(NOT input_bytes EQUAL 65537)
    message(FATAL_ERROR "Input-limit fixture is not exactly 65537 bytes")
endif()
reject_limit(input-65537 "${input}" "manifest exceeds 65536 bytes" 65536)

# A long decimal version is legal metadata, not an overflowing machine Int.
# Thus these strings are genuinely accepted rather than failing a name/path
# rule after a successful scan. ASCII characters are exactly one decoded byte.
foreach(size IN ITEMS 4096 4097)
    math(EXPR digits "${size} - 4")
    string(REPEAT "1" "${digits}" major)
    set(version "${major}.0.0")
    string(LENGTH "${version}" decoded_bytes)
    if(NOT decoded_bytes EQUAL size)
        message(FATAL_ERROR "Decoded-string fixture has the wrong byte size")
    endif()
    foreach(form IN ITEMS literal escaped-u escaped-U)
        if(form STREQUAL "literal")
            set(encoded "'${version}'")
        elseif(form STREQUAL "escaped-u")
            # Overflow at an ESCAPE, after many decoded pieces.
            string(REPEAT [=[\u0031]=] "${digits}" escaped)
            set(encoded "\"${escaped}\\u002E\\u0030\\u002E\\u0030\"")
        else()
            # Overflow in the RAW TAIL, after wide Unicode escapes.
            string(REPEAT [=[\U00000031]=] "${digits}" escaped)
            set(encoded "\"${escaped}.0.0\"")
        endif()
        string(REPLACE "version = '0.1.0'" "version = ${encoded}" input "${base}")
        string(FIND "${input}" "${encoded}" value_pos)
        if(size EQUAL 4096)
            accept_limit("string-${form}-${size}" "${input}" 1 1 1 "${version}")
        else()
            reject_limit("string-${form}-${size}" "${input}"
                "decoded string exceeds 4096 bytes" "${value_pos}")
        endif()
    endforeach()
endforeach()

# Sources are unique: repeated entries would otherwise reject before the bound.
set(entries "")
foreach(index RANGE 1 1024)
    if(NOT entries STREQUAL "")
        string(APPEND entries ", ")
    endif()
    string(APPEND entries "'file-${index}.joyeer'")
endforeach()
string(REPLACE "['main.joyeer']" "[${entries}]" input "${base}")
accept_limit(array-1024 "${input}" 1 1 1024 "0.1.0")
string(APPEND entries ", 'file-1025.joyeer'")
string(REPLACE "['main.joyeer']" "[${entries}]" input "${base}")
string(FIND "${input}" "'file-1025.joyeer'" item_pos)
reject_limit(array-1025 "${input}" "array exceeds 1024 entries" "${item_pos}")

# All names are distinct and every target/dependency resolves. At 256 the
# parse must succeed through full graph validation; at 257 it fails at a header.
set(input "${header}")
foreach(index RANGE 1 256)
    string(APPEND input "[[modules]]\nname = 'limits.m${index}'\nsources = ['main.joyeer']\ndependencies = []\n")
endforeach()
string(REPLACE "limits.app" "limits.m1" first_target "${target}")
string(APPEND input "${first_target}")
accept_limit(modules-256 "${input}" 256 1 1 "0.1.0")
string(LENGTH "${input}" table_pos)
string(APPEND input "[[modules]]\nname = 'limits.m257'\nsources = ['main.joyeer']\ndependencies = []\n")
reject_limit(modules-257 "${input}" "manifest exceeds 256 modules" "${table_pos}")

set(input "${header}${module}")
foreach(index RANGE 1 256)
    string(APPEND input "[[targets]]\nname = 'target-${index}'\nkind = 'bin'\nmodule = 'limits.app'\n")
endforeach()
accept_limit(targets-256 "${input}" 1 256 1 "0.1.0")
string(LENGTH "${input}" table_pos)
string(APPEND input "[[targets]]\nname = 'target-257'\nkind = 'bin'\nmodule = 'limits.app'\n")
reject_limit(targets-257 "${input}" "manifest exceeds 256 targets" "${table_pos}")
message(STATUS "joypm 14 direct manifest-limit cases and 7 oversized CLI diagnostics passed")