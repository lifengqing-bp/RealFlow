cmake_minimum_required(VERSION 3.20)
project(RealFlow VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(byteturn
  src/agent.cpp
  src/conversation.cpp
  src/conversation_session.cpp
  src/event.cpp
  src/event_timeline.cpp
  src/executor.cpp
  src/full_duplex_conversation.cpp
  src/pipeline_conversation_engine.cpp
  src/openai_compatible.cpp
  src/session.cpp
  src/observability.cpp
  src/curl_transport.cpp
  src/sentence_segmenter.cpp
  src/incremental_tts.cpp
  src/turn_context.cpp
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

add_executable(runtime_foundation_tests tests/runtime_foundation_tests.cpp)
target_link_libraries(runtime_foundation_tests PRIVATE byteturn)
add_test(NAME runtime_foundation_tests COMMAND runtime_foundation_tests)
set_tests_properties(byteturn_tests runtime_foundation_tests PROPERTIES TIMEOUT 120)
