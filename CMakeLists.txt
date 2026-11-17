cmake_minimum_required(VERSION 3.20)
project(slipstream LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CheckCXXSourceCompiles)
check_cxx_source_compiles(
  "int main() { __extension__ typedef __int128 Int128; return Int128{0}; }"
  SLIPSTREAM_HAS_INT128
)
if(NOT SLIPSTREAM_HAS_INT128)
  message(FATAL_ERROR "M2 requires a compiler with signed __int128 support (use GCC, Clang, or MinGW-w64 GCC).")
endif()

add_library(slipstream_codec src/codec.cpp)
target_include_directories(slipstream_codec PUBLIC include)
add_library(slipstream_replay src/csv.cpp src/replay.cpp)
target_link_libraries(slipstream_replay PUBLIC slipstream_codec)

if(MSVC)
  set(SLIPSTREAM_WARNINGS /W4 /permissive-)
else()
  set(SLIPSTREAM_WARNINGS -Wall -Wextra -Wpedantic)
endif()
add_compile_options(${SLIPSTREAM_WARNINGS})
target_compile_options(slipstream_codec PRIVATE ${SLIPSTREAM_WARNINGS})
target_compile_options(slipstream_replay PRIVATE ${SLIPSTREAM_WARNINGS})

add_library(slipstream_market STATIC src/market.cpp)
target_link_libraries(slipstream_market PUBLIC slipstream_codec)

add_library(slipstream_tcp STATIC src/tcp_socket.cpp src/tcp_feed.cpp)
target_link_libraries(slipstream_tcp PUBLIC slipstream_replay)
if(WIN32)
  target_link_libraries(slipstream_tcp PRIVATE ws2_32)
endif()

add_executable(slipstream src/server_main.cpp)
target_link_libraries(slipstream PRIVATE slipstream_market slipstream_tcp)

add_executable(md_client src/client_main.cpp)
target_link_libraries(md_client PRIVATE slipstream_tcp)
add_executable(oe_client src/client_main.cpp)
target_compile_definitions(oe_client PRIVATE SLIPSTREAM_OE_CLIENT=1)
target_link_libraries(oe_client PRIVATE slipstream_tcp)

enable_testing()
add_executable(m1_tests tests/m1_tests.cpp)
target_link_libraries(m1_tests PRIVATE slipstream_replay)
add_test(NAME m1_tests COMMAND m1_tests ${CMAKE_CURRENT_SOURCE_DIR}/QandT.csv)

find_package(Threads REQUIRED)
add_executable(m2_tests tests/m2_tests.cpp)
target_include_directories(m2_tests PRIVATE src)
target_link_libraries(m2_tests PRIVATE slipstream_market slipstream_tcp Threads::Threads)
add_test(NAME m2_tests COMMAND m2_tests "${CMAKE_CURRENT_SOURCE_DIR}/QandT.csv")
set_tests_properties(m2_tests PROPERTIES TIMEOUT 15)
