.PHONY: all test test-foundation test-runtime test-interaction test-observability test-unit test-integration clean
CXX ?= c++
CXXFLAGS := -std=c++17 -Wall -Wextra -Wpedantic -O2 -pthread -Iinclude
SOURCES := src/runtime_manager.cpp src/agent.cpp src/conversation.cpp src/conversation_session.cpp src/event.cpp src/event_timeline.cpp src/executor.cpp src/full_duplex_conversation.cpp src/pipeline_conversation_engine.cpp src/openai_compatible.cpp src/session.cpp src/observability.cpp src/curl_transport.cpp src/sentence_segmenter.cpp src/incremental_tts.cpp src/turn_context.cpp
OBJECTS := $(SOURCES:src/%.cpp=build/obj/%.o)
HEADERS := $(wildcard include/byteturn/*.h)
TEST_HEADERS := tests/test_support.h
PYTHON ?= python3
FOUNDATION_SOURCES := src/event.cpp src/event_timeline.cpp src/conversation_session.cpp
LDLIBS := -lcurl

all: build/byteturn_cli build/runtime_manager_demo

build:
	mkdir -p build

build/obj/%.o: src/%.cpp $(HEADERS) | build
	mkdir -p build/obj
	$(CXX) $(CXXFLAGS) -c $< -o $@

build/byteturn_cli: $(OBJECTS) apps/byteturn_cli.cpp $(HEADERS) | build
	$(CXX) $(CXXFLAGS) $(filter %.cpp %.o,$^) -o $@ $(LDLIBS)

build/byteturn_tests: $(OBJECTS) tests/byteturn_tests.cpp $(HEADERS) | build
	$(CXX) $(CXXFLAGS) $(filter %.cpp %.o,$^) -o $@ $(LDLIBS)

build/runtime_foundation_tests: $(FOUNDATION_SOURCES) tests/runtime_foundation_tests.cpp $(HEADERS) | build
	$(CXX) $(CXXFLAGS) $(filter %.cpp %.o,$^) -o $@

test-foundation: build/runtime_foundation_tests
	./build/runtime_foundation_tests

build/runtime_manager_tests: $(FOUNDATION_SOURCES) src/runtime_manager.cpp tests/runtime_manager_tests.cpp $(HEADERS) | build
	$(CXX) $(CXXFLAGS) $(filter %.cpp %.o,$^) -o $@

build/runtime_manager_demo: $(FOUNDATION_SOURCES) src/runtime_manager.cpp apps/runtime_manager_demo.cpp $(HEADERS) | build
	$(CXX) $(CXXFLAGS) $(filter %.cpp %.o,$^) -o $@

test-runtime: build/runtime_manager_tests
	./build/runtime_manager_tests

build/interaction_control_tests: $(OBJECTS) tests/interaction_control_tests.cpp $(HEADERS) | build
	$(CXX) $(CXXFLAGS) $(filter %.cpp %.o,$^) -o $@ $(LDLIBS)

test-interaction: build/interaction_control_tests
	./build/interaction_control_tests

build/observability_contract_tests: $(OBJECTS) tests/observability_contract_tests.cpp $(HEADERS) | build
	$(CXX) $(CXXFLAGS) $(filter %.cpp %.o,$^) -o $@ $(LDLIBS)

test-observability: build/observability_contract_tests
	./build/observability_contract_tests

build/component_unit_tests: $(OBJECTS) tests/component_unit_tests.cpp $(HEADERS) $(TEST_HEADERS) | build
	$(CXX) $(CXXFLAGS) $(filter %.cpp %.o,$^) -o $@ $(LDLIBS)

build/conversation_integration_tests: $(OBJECTS) tests/conversation_integration_tests.cpp $(HEADERS) $(TEST_HEADERS) | build
	$(CXX) $(CXXFLAGS) $(filter %.cpp %.o,$^) -o $@ $(LDLIBS)

build/http_transport_integration_tests: $(OBJECTS) tests/http_transport_integration_tests.cpp $(HEADERS) $(TEST_HEADERS) | build
	$(CXX) $(CXXFLAGS) $(filter %.cpp %.o,$^) -o $@ $(LDLIBS)

test-unit: build/component_unit_tests build/runtime_foundation_tests build/runtime_manager_tests
	./build/component_unit_tests
	./build/runtime_foundation_tests
	./build/runtime_manager_tests

test-integration: build/conversation_integration_tests build/http_transport_integration_tests build/interaction_control_tests build/observability_contract_tests
	./build/conversation_integration_tests
	$(PYTHON) tests/http_fixture.py build/http_transport_integration_tests
	./build/interaction_control_tests
	./build/observability_contract_tests

test: build/component_unit_tests build/conversation_integration_tests build/http_transport_integration_tests build/observability_contract_tests build/byteturn_tests build/runtime_foundation_tests build/runtime_manager_tests build/interaction_control_tests
	./build/observability_contract_tests
	./build/byteturn_tests
	./build/runtime_foundation_tests
	./build/runtime_manager_tests
	./build/interaction_control_tests
	./build/component_unit_tests
	./build/conversation_integration_tests
	$(PYTHON) tests/http_fixture.py build/http_transport_integration_tests

clean:
	rm -rf build

# Optional provider: set JSON_INCLUDE_DIR to the parent of nlohmann/json.hpp.
JSON_INCLUDE_DIR ?= /usr/include
.PHONY: test-byteplus
build/byteplus_tts_tests: src/byteplus_tts.cpp tests/byteplus_tts_tests.cpp $(HEADERS) | build
	$(CXX) $(CXXFLAGS) -isystem $(JSON_INCLUDE_DIR) $(filter %.cpp,$^) -o $@

build/byteplus_tts_demo: src/byteplus_tts.cpp src/curl_transport.cpp src/observability.cpp src/event.cpp src/event_timeline.cpp apps/byteplus_tts_demo.cpp $(HEADERS) | build
	$(CXX) $(CXXFLAGS) -isystem $(JSON_INCLUDE_DIR) $(filter %.cpp,$^) -o $@ $(LDLIBS)

test-byteplus: build/byteplus_tts_tests
	./build/byteplus_tts_tests
