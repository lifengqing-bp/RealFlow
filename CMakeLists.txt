cmake_minimum_required(VERSION 3.20)
project(ByteTurn VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(byteturn
  src/agent.cpp
  src/conversation.cpp
  src/event.cpp
  src/executor.cpp
  src/openai_compatible.cpp
  src/session.cpp
  src/observability.cpp
  src/curl_transport.cpp
  src/sentence_segmenter.cpp
  src/incremental_tts.cpp
)
find_package(Threads REQUIRED)
find_package(CURL REQUIRED)
target_link_libraries(byteturn PUBLIC Threads::Threads CURL::libcurl)
target_include_directories(byteturn PUBLIC include)
target_compile_options(byteturn PRIVATE -Wall -Wextra -Wpedantic)

add_executable(byteturn_cli apps/byteturn_cli.cpp)
target_link_libraries(byteturn_cli PRIVATE byteturn)

enable_testing()
add_executable(byteturn_tests tests/byteturn_tests.cpp)
target_link_libraries(byteturn_tests PRIVATE byteturn)
add_test(NAME byteturn_tests COMMAND byteturn_tests)
