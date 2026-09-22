cmake_minimum_required(VERSION 3.20)
project(RealFlow VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(byteturn
  src/mock_providers.cpp
  src/runtime_manager.cpp
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

add_executable(runtime_manager_tests tests/runtime_manager_tests.cpp)
target_link_libraries(runtime_manager_tests PRIVATE byteturn)
add_test(NAME runtime_manager_tests COMMAND runtime_manager_tests)
set_tests_properties(runtime_manager_tests PROPERTIES TIMEOUT 120)

add_executable(runtime_manager_demo apps/runtime_manager_demo.cpp)
target_link_libraries(runtime_manager_demo PRIVATE byteturn)
add_test(NAME runtime_manager_demo COMMAND runtime_manager_demo)
set_tests_properties(runtime_manager_demo PROPERTIES TIMEOUT 30)

add_executable(interaction_control_tests tests/interaction_control_tests.cpp)
target_link_libraries(interaction_control_tests PRIVATE byteturn)
add_test(NAME interaction_control_tests COMMAND interaction_control_tests)
set_tests_properties(interaction_control_tests PROPERTIES TIMEOUT 60)

add_executable(observability_contract_tests tests/observability_contract_tests.cpp)
target_link_libraries(observability_contract_tests PRIVATE byteturn)
add_test(NAME observability_contract_tests COMMAND observability_contract_tests)
set_tests_properties(observability_contract_tests PROPERTIES TIMEOUT 60)

add_executable(mock_provider_tests tests/mock_provider_tests.cpp)
target_link_libraries(mock_provider_tests PRIVATE byteturn)
add_test(NAME mock_provider_tests COMMAND mock_provider_tests)
add_executable(mock_pipeline_demo apps/mock_pipeline_demo.cpp)
target_link_libraries(mock_pipeline_demo PRIVATE byteturn)
add_test(NAME mock_pipeline_demo COMMAND mock_pipeline_demo)
set_tests_properties(mock_provider_tests mock_pipeline_demo PROPERTIES TIMEOUT 30)
