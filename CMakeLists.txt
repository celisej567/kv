cmake_minimum_required(VERSION 3.14)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
project(kv CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(KV_THREAD_SAFE "Guard the global string pool with a mutex" OFF)
option(KV_VERBOSE "Log what a load and a write do to stderr (errors are always printed)" OFF)
option(KV_SANITIZE "Build with AddressSanitizer + UBSan (GCC/Clang)" OFF)
option(KV_TSAN "Build with ThreadSanitizer (GCC/Clang, exclusive with KV_SANITIZE)" OFF)

set(KV_SOURCES
    src/kv_string_pool.cpp
    src/keyvalue.cpp
    src/kv_parser.cpp
    src/kv_writer.cpp)

add_library(kv STATIC ${KV_SOURCES})
target_include_directories(kv PUBLIC src)

# Same code with the pool mutex always on; used by the multithreading tests.
add_library(kv_mt STATIC ${KV_SOURCES})
target_include_directories(kv_mt PUBLIC src)
target_compile_definitions(kv_mt PUBLIC KV_THREAD_SAFE)

find_package(Threads REQUIRED)
target_link_libraries(kv_mt PUBLIC Threads::Threads)

if(KV_THREAD_SAFE)
    target_compile_definitions(kv PUBLIC KV_THREAD_SAFE)
    target_link_libraries(kv PUBLIC Threads::Threads)
endif()

foreach(lib kv kv_mt)
    if(KV_VERBOSE)
        target_compile_definitions(${lib} PRIVATE KV_VERBOSE)
    endif()
    if(MSVC)
        target_compile_options(${lib} PUBLIC /W4)
    else()
        target_compile_options(${lib} PUBLIC -Wall -Wextra)
        if(KV_SANITIZE)
            target_compile_options(${lib} PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
            target_link_options(${lib} PUBLIC -fsanitize=address,undefined)
        elseif(KV_TSAN)
            target_compile_options(${lib} PUBLIC -fsanitize=thread -fno-omit-frame-pointer)
            target_link_options(${lib} PUBLIC -fsanitize=thread)
        endif()
    endif()
endforeach()

add_executable(kv_cli tools/kv_cli.cpp)
target_link_libraries(kv_cli PRIVATE kv)
if(KV_VERBOSE)
    target_compile_definitions(kv_cli PRIVATE KV_VERBOSE)
endif()
if(WIN32)
    target_link_libraries(kv_cli PRIVATE psapi)
endif()
# std::filesystem (temp files of the API scenarios) is a separate library in GCC before 9
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.0)
    target_link_libraries(kv_cli PRIVATE stdc++fs)
endif()

# Pool memory/threading driver, built against both library variants (see tools/kv_stress.cpp).
add_executable(kv_stress tools/kv_stress.cpp)
target_link_libraries(kv_stress PRIVATE kv_mt)
add_executable(kv_stress_st tools/kv_stress.cpp)
target_link_libraries(kv_stress_st PRIVATE kv)

# Pool design comparison (not a test): kv_pool_bench [st|mt|all] [scale]
add_executable(kv_pool_bench bench/kv_pool_bench.cpp)
target_link_libraries(kv_pool_bench PRIVATE kv Threads::Threads)

add_executable(kv_example example/main.cpp)
target_link_libraries(kv_example PRIVATE kv)

enable_testing()
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_test(NAME cli_cases COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_tests.py $<TARGET_FILE:kv_cli>)
    set_tests_properties(cli_cases PROPERTIES TIMEOUT 900)
    add_dependencies(kv_cli kv_stress kv_stress_st)  # run_tests.py finds them next to kv_cli
endif()
