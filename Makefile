.PHONY: all test clean
CXX ?= c++
CXXFLAGS := -std=c++17 -Wall -Wextra -Wpedantic -O2 -Iinclude
SOURCES := src/agent.cpp src/conversation.cpp

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

