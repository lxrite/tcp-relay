cmake_minimum_required(VERSION 3.5)
project(tcp-relay)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_definitions(-DASIO_STANDALONE)

set(ASIO_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/asio/asio/include)

include_directories(${ASIO_INCLUDE_DIR})

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  if (CMAKE_CXX_COMPILER MATCHES ".*musl\\-g\\+\\+.*")
    # fix openwrt build issue
    add_definitions(-DASIO_HAS_CO_AWAIT=1)
    add_definitions(-DASIO_HAS_STD_COROUTINE=1)
  endif()
endif()

add_executable(tcp-relay src/tcp_relay.cpp)
if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
  target_compile_options(tcp-relay PRIVATE -fcoroutines)
endif()

install(TARGETS tcp-relay DESTINATION bin)
