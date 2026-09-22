cmake_minimum_required(VERSION 3.20)
project(RealFlow VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(byteturn
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

# Fast component checks and cross-component runtime/protocol integration.
add_executable(component_unit_tests tests/component_unit_tests.cpp)
target_link_libraries(component_unit_tests PRIVATE byteturn)
target_compile_options(component_unit_tests PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME component_unit_tests COMMAND component_unit_tests)
set_tests_properties(component_unit_tests PROPERTIES LABELS "unit" TIMEOUT 60)

add_executable(conversation_integration_tests tests/conversation_integration_tests.cpp)
target_link_libraries(conversation_integration_tests PRIVATE byteturn)
target_compile_options(conversation_integration_tests PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME conversation_integration_tests COMMAND conversation_integration_tests)
set_tests_properties(conversation_integration_tests PROPERTIES LABELS "integration" TIMEOUT 90)

option(REALFLOW_ENABLE_HTTP_TESTS "Run loopback HTTP integration tests (requires Python 3.9+)" ON)
if(REALFLOW_ENABLE_HTTP_TESTS)
  find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
  add_executable(http_transport_integration_tests tests/http_transport_integration_tests.cpp)
  target_link_libraries(http_transport_integration_tests PRIVATE byteturn)
  target_compile_options(http_transport_integration_tests PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME http_transport_integration_tests
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/http_fixture.py
            $<TARGET_FILE:http_transport_integration_tests>)
  set_tests_properties(http_transport_integration_tests PROPERTIES LABELS "integration;http" TIMEOUT 90)
endif()

set_tests_properties(runtime_foundation_tests runtime_manager_tests
  PROPERTIES LABELS "unit;concurrency")
set_tests_properties(interaction_control_tests observability_contract_tests
  PROPERTIES LABELS "integration;concurrency")
set_tests_properties(byteturn_tests PROPERTIES LABELS "regression")
set_tests_properties(runtime_manager_demo PROPERTIES LABELS "smoke")
