include("${CMAKE_CURRENT_LIST_DIR}/platform_base.cmake")

function(set_source_files)
    set(oneValueArgs RESULT)
    cmake_parse_arguments(ANDROID_SSS "" "${oneValueArgs}" "" ${ARGN})
    if(NOT DEFINED ANDROID_SSS_RESULT)
        message(FATAL_ERROR "set_source_files needs to be called with RESULT")
    endif()

    set_source_files_base(RESULT BASE_SRC_FILES)
    set(
        ${ANDROID_SSS_RESULT}
        ${BASE_SRC_FILES}
        src/util/ThreadedMailbox.cc
        PARENT_SCOPE
    )
endfunction()

function(setup_build)
    add_subdirectory("vendor/zlib")
    target_include_directories(
        BLIPStatic PRIVATE
        vendor/zlib
        ${CMAKE_CURRENT_BINARY_DIR}/vendor/zlib
        ${LITECORE_LOCATION}/LiteCore/Unix
    )
endfunction()