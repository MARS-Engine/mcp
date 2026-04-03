cmake_minimum_required(VERSION 3.30)
project(mcp_compiler LANGUAGES CXX)

include(FetchContent)

FetchContent_Declare(
    json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    URL_HASH SHA256=42F6E95CAD6EC532FD372391373363B62A14AF6D771056DBFC86160E6DFFF7AA
)

FetchContent_MakeAvailable(json)

set(CLANG_DIR "$ENV{CLANG_DIR}" CACHE PATH "Path to your custom Clang install")

add_executable(compiler main.cpp)

target_compile_features(compiler PRIVATE cxx_std_26)
set_target_properties(compiler PROPERTIES CXX_STANDARD_REQUIRED ON)

target_link_libraries(compiler PRIVATE nlohmann_json::nlohmann_json)

target_include_directories(compiler PRIVATE "${CLANG_DIR}/include")

if(WIN32)
    target_link_libraries(compiler PRIVATE "${CLANG_DIR}/lib/libclang.lib" nlohmann_json::nlohmann_json)
else()
    target_link_libraries(compiler PRIVATE "${CLANG_DIR}/lib/libclang.so" nlohmann_json::nlohmann_json)
endif()