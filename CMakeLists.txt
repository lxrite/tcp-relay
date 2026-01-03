cmake_minimum_required(VERSION 3.5)
project(tcp-relay)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_definitions(-DASIO_STANDALONE)

set(ASIO_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/asio/asio/include)
set(MBEDTLS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/mbedtls)

include_directories(${ASIO_INCLUDE_DIR})

# mbedtls configuration
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
add_subdirectory(${MBEDTLS_DIR})
include_directories(${MBEDTLS_DIR}/include)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  if (CMAKE_CXX_COMPILER MATCHES ".*musl\\-g\\+\\+.*")
    # fix openwrt build issue
    add_definitions(-DASIO_HAS_CO_AWAIT=1)
    add_definitions(-DASIO_HAS_STD_COROUTINE=1)
  endif()
endif()

add_executable(tcp-relay src/tcp_relay.cpp)
target_link_libraries(tcp-relay mbedcrypto)
if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
  target_compile_options(tcp-relay PRIVATE -fcoroutines)
endif()

install(TARGETS tcp-relay DESTINATION bin)

# Benchmark tools (optional)
option(BUILD_BENCHMARK "Build benchmark tools" OFF)

if(BUILD_BENCHMARK)
  add_executable(echo-server benchmark/echo_server.cpp)
  add_executable(benchmark-client benchmark/benchmark_client.cpp)

  if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
    target_compile_options(echo-server PRIVATE -fcoroutines)
    target_compile_options(benchmark-client PRIVATE -fcoroutines)
  endif()
endif()
