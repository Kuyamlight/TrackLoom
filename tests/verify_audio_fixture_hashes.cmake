set(required_files
    reference.trackloom
    expected-events.tsv
    expected-audio.properties
    README.md)

function(verify_fixture_manifest fixture_dir)
    if(NOT EXISTS "${fixture_dir}/SHA256SUMS")
        message(FATAL_ERROR "Audio fixture checksum manifest is missing")
    endif()

    file(STRINGS "${fixture_dir}/SHA256SUMS" checksum_lines)
    set(seen_files)
    foreach(checksum_line IN LISTS checksum_lines)
        string(REGEX MATCH "^([0-9a-f]+)  (.+)$" matched "${checksum_line}")
        string(LENGTH "${CMAKE_MATCH_1}" expected_hash_length)
        if(NOT matched OR NOT expected_hash_length EQUAL 64)
            message(FATAL_ERROR "Invalid audio fixture checksum row: ${checksum_line}")
        endif()
        set(expected_hash "${CMAKE_MATCH_1}")
        set(expected_file "${CMAKE_MATCH_2}")
        list(FIND required_files "${expected_file}" required_index)
        if(required_index EQUAL -1)
            message(FATAL_ERROR "Unknown audio fixture checksum file: ${expected_file}")
        endif()
        list(FIND seen_files "${expected_file}" duplicate_index)
        if(NOT duplicate_index EQUAL -1)
            message(FATAL_ERROR "Duplicate audio fixture checksum file: ${expected_file}")
        endif()
        list(APPEND seen_files "${expected_file}")
        if(NOT EXISTS "${fixture_dir}/${expected_file}")
            message(FATAL_ERROR "Audio fixture checksum file is missing: ${expected_file}")
        endif()
        file(SHA256 "${fixture_dir}/${expected_file}" actual_hash)
        if(NOT actual_hash STREQUAL expected_hash)
            message(FATAL_ERROR "Audio fixture hash mismatch: ${expected_file}")
        endif()
    endforeach()

    foreach(required_file IN LISTS required_files)
        list(FIND seen_files "${required_file}" required_index)
        if(required_index EQUAL -1)
            message(FATAL_ERROR "Audio fixture checksum entry is missing: ${required_file}")
        endif()
    endforeach()
endfunction()

function(write_case_manifest case_dir mode)
    file(COPY
        "${REFERENCE_FIXTURE_DIR}/reference.trackloom"
        "${REFERENCE_FIXTURE_DIR}/expected-events.tsv"
        "${REFERENCE_FIXTURE_DIR}/expected-audio.properties"
        "${REFERENCE_FIXTURE_DIR}/README.md"
        DESTINATION "${case_dir}")
    file(READ "${REFERENCE_FIXTURE_DIR}/SHA256SUMS" manifest)
    if(mode STREQUAL "missing")
        string(REGEX REPLACE "[^\n]*  reference\\.trackloom\n" "" manifest "${manifest}")
    elseif(mode STREQUAL "duplicate")
        string(REGEX MATCH "[^\n]*  reference\\.trackloom" duplicate_line "${manifest}")
        string(APPEND manifest "${duplicate_line}\n")
    elseif(mode STREQUAL "unknown")
        string(APPEND manifest "0000000000000000000000000000000000000000000000000000000000000000  unrelated.txt\n")
    elseif(mode STREQUAL "traversal")
        string(APPEND manifest "0000000000000000000000000000000000000000000000000000000000000000  ../README.md\n")
    endif()
    file(WRITE "${case_dir}/SHA256SUMS" "${manifest}")
endfunction()

function(require_rejected_case case_dir case_name)
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
            "-DFIXTURE_DIR=${case_dir}"
            "-DVERIFY_AUDIO_FIXTURE_HASHES_INTERNAL=1"
            -P "${CMAKE_CURRENT_LIST_FILE}"
        RESULT_VARIABLE case_result
        OUTPUT_QUIET
        ERROR_QUIET)
    if(case_result EQUAL 0)
        message(FATAL_ERROR "Audio fixture manifest self-test accepted ${case_name}")
    endif()
endfunction()

if(VERIFY_AUDIO_FIXTURE_HASHES_INTERNAL)
    verify_fixture_manifest("${FIXTURE_DIR}")
    return()
endif()

if(VERIFY_AUDIO_FIXTURE_HASHES_SELF_TEST)
    if(NOT DEFINED REFERENCE_FIXTURE_DIR)
        message(FATAL_ERROR "REFERENCE_FIXTURE_DIR is required for the manifest self-test")
    endif()
    set(self_test_root "${CMAKE_CURRENT_BINARY_DIR}/trackloom-audio-fixture-manifest-self-test")
    file(REMOVE_RECURSE "${self_test_root}")
    foreach(case_name IN ITEMS missing duplicate unknown traversal)
        set(case_dir "${self_test_root}/${case_name}")
        file(MAKE_DIRECTORY "${case_dir}")
        write_case_manifest("${case_dir}" "${case_name}")
        require_rejected_case("${case_dir}" "${case_name}")
    endforeach()
    file(REMOVE_RECURSE "${self_test_root}")
    message(STATUS "Audio fixture manifest self-test passed")
    return()
endif()

if(NOT DEFINED FIXTURE_DIR)
    message(FATAL_ERROR "FIXTURE_DIR is required")
endif()
verify_fixture_manifest("${FIXTURE_DIR}")
