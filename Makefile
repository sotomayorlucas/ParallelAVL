# Makefile for Parallel AVL Tree Subproject

CXX = g++
CXXFLAGS = -std=c++17 -O3 -Wall -Wextra -pthread
INCLUDES = -I include

# Targets
.PHONY: all benchmarks tests clean paper help

all: benchmarks tests

# Main benchmarks
benchmarks: benchmark_parallel benchmark_routing

benchmark_parallel: benchmark_parallel_trees.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< -o $@

benchmark_routing: benchmark_routing_strategies.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< -o $@

# Bench directory benchmarks
bench_rigorous: bench/rigorous_bench.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< -o $@

bench_throughput: bench/throughput_bench.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< -o $@

bench_adversarial: bench/adversarial_bench.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< -o $@

# Tests
tests: test_linearizability test_workloads

test_linearizability: tests/linearizability_test.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< -o $@

test_workloads: tests/workloads_test.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< -o $@

# Paper
paper:
	$(MAKE) -C paper

# Run all tests
run-tests: tests
	./test_linearizability
	./test_workloads

# Run all benchmarks
run-benchmarks: benchmarks
	./benchmark_parallel
	./benchmark_routing

# Clean
clean:
	rm -f benchmark_parallel benchmark_routing
	rm -f bench_rigorous bench_throughput bench_adversarial
	rm -f test_linearizability test_workloads
	$(MAKE) -C paper clean

help:
	@echo "Parallel AVL Tree - Build System"
	@echo ""
	@echo "Targets:"
	@echo "  make              - Build benchmarks and tests"
	@echo "  make benchmarks   - Build main benchmarks"
	@echo "  make tests        - Build tests"
	@echo "  make paper        - Compile academic paper"
	@echo "  make run-tests    - Run all tests"
	@echo "  make run-benchmarks - Run all benchmarks"
	@echo "  make clean        - Clean build artifacts"
	@echo ""
