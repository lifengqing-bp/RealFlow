.PHONY: all test clean
CXX ?= c++
CXXFLAGS := -std=c++17 -Wall -Wextra -Wpedantic -O2 -pthread -Iinclude
SOURCES := src/agent.cpp src/conversation.cpp src/event.cpp src/executor.cpp src/openai_compatible.cpp src/session.cpp

all: build/byteturn_cli

build:
	mkdir -p build

build/byteturn_cli: $(SOURCES) apps/byteturn_cli.cpp | build
	$(CXX) $(CXXFLAGS) $^ -o $@

build/byteturn_tests: $(SOURCES) tests/byteturn_tests.cpp | build
	$(CXX) $(CXXFLAGS) $^ -o $@

test: build/byteturn_tests
	./build/byteturn_tests

clean:
	rm -rf build
