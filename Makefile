.PHONY: all test test-foundation test-runtime clean
CXX ?= c++
CXXFLAGS := -std=c++17 -Wall -Wextra -Wpedantic -O2 -pthread -Iinclude
SOURCES := src/runtime_manager.cpp src/agent.cpp src/conversation.cpp src/conversation_session.cpp src/event.cpp src/event_timeline.cpp src/executor.cpp src/full_duplex_conversation.cpp src/pipeline_conversation_engine.cpp src/openai_compatible.cpp src/session.cpp src/observability.cpp src/curl_transport.cpp src/sentence_segmenter.cpp src/incremental_tts.cpp src/turn_context.cpp
HEADERS := $(wildcard include/byteturn/*.h)
FOUNDATION_SOURCES := src/event.cpp src/event_timeline.cpp src/conversation_session.cpp
LDLIBS := -lcurl

all: build/byteturn_cli build/runtime_manager_demo

build:
	mkdir -p build

build/byteturn_cli: $(SOURCES) apps/byteturn_cli.cpp $(HEADERS) | build
	$(CXX) $(CXXFLAGS) $(filter %.cpp,$^) -o $@ $(LDLIBS)

build/byteturn_tests: $(SOURCES) tests/byteturn_tests.cpp $(HEADERS) | build
	$(CXX) $(CXXFLAGS) $(filter %.cpp,$^) -o $@ $(LDLIBS)

build/runtime_foundation_tests: $(FOUNDATION_SOURCES) tests/runtime_foundation_tests.cpp $(HEADERS) | build
	$(CXX) $(CXXFLAGS) $(filter %.cpp,$^) -o $@

test-foundation: build/runtime_foundation_tests
	./build/runtime_foundation_tests

build/runtime_manager_tests: $(FOUNDATION_SOURCES) src/runtime_manager.cpp tests/runtime_manager_tests.cpp $(HEADERS) | build
	$(CXX) $(CXXFLAGS) $(filter %.cpp,$^) -o $@

build/runtime_manager_demo: $(FOUNDATION_SOURCES) src/runtime_manager.cpp apps/runtime_manager_demo.cpp $(HEADERS) | build
	$(CXX) $(CXXFLAGS) $(filter %.cpp,$^) -o $@

test-runtime: build/runtime_manager_tests
	./build/runtime_manager_tests

test: build/byteturn_tests build/runtime_foundation_tests build/runtime_manager_tests
	./build/byteturn_tests
	./build/runtime_foundation_tests
	./build/runtime_manager_tests

clean:
	rm -rf build
