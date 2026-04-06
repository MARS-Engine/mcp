cmake_minimum_required(VERSION 3.30)

message(STATUS "Toolchain file: ${CMAKE_TOOLCHAIN_FILE}")
message(STATUS "Compiler being used: ${CMAKE_CXX_COMPILER}")

project(mcp_compiler LANGUAGES CXX)

include(FetchContent)

FetchContent_Declare(
    json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    URL_HASH SHA256=42F6E95CAD6EC532FD372391373363B62A14AF6D771056DBFC86160E6DFFF7AA
)

FetchContent_MakeAvailable(json)

set(CLANG_DIR "$ENV{CLANG_DIR}" CACHE PATH "Path to your custom Clang install")

if(NOT CLANG_DIR AND CUSTOM_CLANG_PATH)
    set(CLANG_DIR "${CUSTOM_CLANG_PATH}" CACHE PATH "Path to your custom Clang install" FORCE)
endif()

if(NOT CLANG_DIR AND CMAKE_CXX_COMPILER)
    cmake_path(GET CMAKE_CXX_COMPILER FILENAME _custom_clang_name)
    if(_custom_clang_name MATCHES "^clang([+][+]|-cl)?(\\.exe)?$")
        cmake_path(GET CMAKE_CXX_COMPILER PARENT_PATH _custom_clang_bin_dir)
        cmake_path(GET _custom_clang_bin_dir PARENT_PATH _custom_clang_root)
        set(CLANG_DIR "${_custom_clang_root}" CACHE PATH "Path to your custom Clang install" FORCE)
    endif()
endif()

if(NOT CLANG_DIR)
    message(FATAL_ERROR "mcp_compiler: unable to resolve the custom Clang install. Set CLANG_DIR or configure with a toolchain that defines CUSTOM_CLANG_PATH.")
endif()

find_path(
    CLANG_INCLUDE_DIR
    NAMES clang-c/Index.h
    PATHS "${CLANG_DIR}/include"
    NO_DEFAULT_PATH
)

if(WIN32)
    set(CLANG_LIBRARY_CANDIDATES
        "${CLANG_DIR}/lib/libclang.lib"
        "${CLANG_DIR}/lib/clang.lib"
        "${CLANG_DIR}/bin/libclang.lib"
    )
else()
    set(CLANG_LIBRARY_CANDIDATES
        "${CLANG_DIR}/lib/libclang.so"
        "${CLANG_DIR}/lib/libclang.dylib"
    )
endif()

foreach(_clang_library_candidate IN LISTS CLANG_LIBRARY_CANDIDATES)
    if(EXISTS "${_clang_library_candidate}")
        set(CLANG_LIBRARY "${_clang_library_candidate}")
        break()
    endif()
endforeach()

if(NOT CLANG_INCLUDE_DIR)
    message(FATAL_ERROR "mcp_compiler: clang-c headers were not found under ${CLANG_DIR}/include.")
endif()

if(NOT CLANG_LIBRARY)
    message(FATAL_ERROR "mcp_compiler: libclang was not found under ${CLANG_DIR}.")
endif()

add_executable(compiler main.cpp)

target_compile_features(compiler PRIVATE cxx_std_26)
set_target_properties(compiler PROPERTIES CXX_STANDARD_REQUIRED ON)

target_link_libraries(compiler PRIVATE
    nlohmann_json::nlohmann_json
    "${CLANG_LIBRARY}"
)

target_include_directories(compiler PRIVATE "${CLANG_INCLUDE_DIR}")

if(UNIX AND NOT APPLE)
    if(EXISTS "${CLANG_DIR}/include/c++/v1")
        target_compile_options(compiler PRIVATE "-nostdinc++")
        target_include_directories(compiler PRIVATE "${CLANG_DIR}/include/c++/v1")
        target_link_options(compiler PRIVATE
            "-L${CLANG_DIR}/lib"
            "-stdlib=libc++"
        )
    endif()

    # Compute a relative path from the binary's build directory to the clang lib dir.
    # This makes the RPATH portable: the same relative offset works whether the workspace
    # is mounted at /home/user/... on the host or at /workspace in a container.
    file(RELATIVE_PATH _rel_clang_lib "${CMAKE_CURRENT_BINARY_DIR}" "${CLANG_DIR}/lib")
    set(_compiler_rpath "$ORIGIN")
    list(APPEND _compiler_rpath "$ORIGIN/${_rel_clang_lib}")
    if(EXISTS "${CLANG_DIR}/lib/x86_64-unknown-linux-gnu")
        file(RELATIVE_PATH _rel_clang_lib_arch "${CMAKE_CURRENT_BINARY_DIR}" "${CLANG_DIR}/lib/x86_64-unknown-linux-gnu")
        list(APPEND _compiler_rpath "$ORIGIN/${_rel_clang_lib_arch}")
    endif()

    set_target_properties(compiler PROPERTIES
        BUILD_WITH_INSTALL_RPATH TRUE
        INSTALL_RPATH "${_compiler_rpath}"
        LINK_FLAGS "-Wl,--disable-new-dtags"
    )
endif()
