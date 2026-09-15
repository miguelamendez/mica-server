cmake_minimum_required(VERSION 3.24)
project(mica_server VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(MICA_SERVER_BUILD_TESTS "Build mica-server tests" ON)

find_path(MICA_LUA_INCLUDE_DIR lua.h
  HINTS
    /opt/homebrew/opt/lua/include/lua
    /opt/homebrew/opt/lua/include/lua5.5
  PATH_SUFFIXES lua5.5 lua5.4 lua54 lua
)
find_library(MICA_LUA_LIBRARY
  NAMES lua lua5.5 lua5.4 lua54
  HINTS /opt/homebrew/opt/lua/lib
)
if(NOT MICA_LUA_INCLUDE_DIR OR NOT MICA_LUA_LIBRARY)
  message(FATAL_ERROR "Lua 5.4+ development headers and library are required")
endif()

set(MICA_LLAMA_VENDOR_HINT "${CMAKE_CURRENT_SOURCE_DIR}/../llama.cpp/vendor" CACHE PATH
    "Optional llama.cpp vendor directory containing nlohmann and cpp-httplib")
find_path(MICA_JSON_INCLUDE_DIR nlohmann/json.hpp HINTS "${MICA_LLAMA_VENDOR_HINT}")
find_path(MICA_HTTPLIB_INCLUDE_DIR httplib.h HINTS "${MICA_LLAMA_VENDOR_HINT}/cpp-httplib")

include(FetchContent)
if(NOT MICA_JSON_INCLUDE_DIR)
  FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    URL_HASH SHA256=d6c65aca6b1ed68b4e7f541b3967fb4c52a0c6f9c7bf3e6ec8ef52d24f3a33d1
  )
  FetchContent_MakeAvailable(nlohmann_json)
endif()

if(NOT MICA_HTTPLIB_INCLUDE_DIR)
  FetchContent_Declare(
    cpp_httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG v0.18.3
    GIT_SHALLOW TRUE
  )
  FetchContent_MakeAvailable(cpp_httplib)
endif()

add_library(mica_server_core
  src/catalog.cpp
  src/command.cpp
  src/config.cpp
  src/hardware.cpp
  src/scheduler.cpp
  src/setup.cpp
)
target_include_directories(mica_server_core
  PUBLIC include
  PRIVATE ${MICA_LUA_INCLUDE_DIR}
)
target_link_libraries(mica_server_core PUBLIC ${MICA_LUA_LIBRARY})

if(MICA_JSON_INCLUDE_DIR)
  target_include_directories(mica_server_core PUBLIC "${MICA_JSON_INCLUDE_DIR}")
else()
  target_link_libraries(mica_server_core PUBLIC nlohmann_json::nlohmann_json)
endif()

if(MICA_HTTPLIB_INCLUDE_DIR)
  target_include_directories(mica_server_core PUBLIC "${MICA_HTTPLIB_INCLUDE_DIR}")
else()
  target_link_libraries(mica_server_core PUBLIC httplib::httplib)
endif()

if(APPLE)
  target_link_libraries(mica_server_core PRIVATE "-framework Security")
endif()

target_compile_options(mica_server_core PRIVATE -Wall -Wextra -Wpedantic)

add_executable(mica-server src/main.cpp src/server.cpp)
target_link_libraries(mica-server PRIVATE mica_server_core)

if(MICA_SERVER_BUILD_TESTS)
  enable_testing()
  add_executable(mica-server-tests tests/test_scheduler.cpp)
  target_link_libraries(mica-server-tests PRIVATE mica_server_core)
  add_test(NAME mica-server-tests COMMAND mica-server-tests)
  add_test(
    NAME mica-server-quantize-plan
    COMMAND $<TARGET_FILE:mica-server> quantize --model fixture-asr --backend gguf
            --quant q8 --dry-run --root
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/custom-root
  )
  set_tests_properties(mica-server-quantize-plan PROPERTIES
    PASS_REGULAR_EXPRESSION "audiocpp_gguf.*--type q8_0"
  )
  add_test(
    NAME mica-server-reject-both-serve
    COMMAND $<TARGET_FILE:mica-server> serve --backend both --root
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/custom-root
  )
  set_tests_properties(mica-server-reject-both-serve PROPERTIES
    WILL_FAIL TRUE
  )
endif()

install(TARGETS mica-server RUNTIME DESTINATION bin)
install(DIRECTORY config/ DESTINATION share/mica-server/config)
