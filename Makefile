# Makefile — ThreadPoolV3 (fallback when CMake unavailable)
# =========================================================
# Prefer CMake for full build. Use this for quick compilation.
#
# Usage:
#   make              # build all
#   make test         # run tests (requires GTest)
#   make demo         # run live demo
#   make clean

CXX      := g++
CXXFLAGS := -std=c++17 -O2 -pthread -Iinclude -Wall -Wextra -Wno-interference-size
DBGFLAGS := -g -O0 -fsanitize=thread -fno-omit-frame-pointer

.PHONY: all demo benchmark clean tsan

all: demo benchmark

demo: examples/demo.cpp include/metrics.h include/threadpool_v3.h include/metrics_server.h
	$(CXX) $(CXXFLAGS) -o $@ $<
	@echo "Built: ./demo"
	@echo "Run:   ./demo   (then curl http://localhost:9090/metrics)"

benchmark: examples/benchmark.cpp include/lockfree_queue.h include/threadpool_v2.h
	$(CXX) $(CXXFLAGS) -o $@ $<

# Build with ThreadSanitizer (catches data races)
tsan: examples/demo.cpp
	$(CXX) $(CXXFLAGS) $(DBGFLAGS) -o demo_tsan $<

clean:
	rm -f demo benchmark demo_tsan
	rm -rf build/

# CMake build (preferred)
cmake-build:
	mkdir -p build && cd build && \
	cmake .. -DCMAKE_BUILD_TYPE=Release && \
	make -j$$(nproc)

cmake-test:
	cd build && ctest --output-on-failure -V
