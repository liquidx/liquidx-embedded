# Native tests for the blat firmware library. Needs g++ (C++20), mbedTLS
# (Debian/Ubuntu: libmbedtls-dev) and, for host_test.py, Python 3 with the
# `cryptography` package.
#
#   make            build and run everything
#   make sim        just the simulated device (build/sim_device)

CXX ?= g++
CXXFLAGS ?= -std=c++20 -O1 -g -Wall -Wextra -Wno-unused-parameter -Werror
CPPFLAGS += -I../src
LDLIBS += -lmbedcrypto

LIB_SRCS := ../src/blat/Values.cpp ../src/blat/Schema.cpp ../src/blat/Device.cpp ../src/blat/Crypto.cpp ../src/blat/Wire.cpp
BUILD := build

.PHONY: all test sim bad-tables clean
all: test

$(BUILD):
	mkdir -p $@

$(BUILD)/test_values: test_values.cpp $(LIB_SRCS) $(wildcard ../src/blat/*.h) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ test_values.cpp $(LIB_SRCS) $(LDLIBS)

$(BUILD)/sim_device: sim_device.cpp $(LIB_SRCS) $(wildcard ../src/blat/*.h) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ sim_device.cpp $(LIB_SRCS) $(LDLIBS)

sim: $(BUILD)/sim_device

# Each case must fail to compile, naming the expected error.
bad-tables:
	@for n in 1 2 3 4 5 6 7 8 9; do \
	  want=$$(sed -n "s/^#.*CASE == $$n  \/\/ expect: //p" bad_tables.cpp); \
	  if $(CXX) $(CXXFLAGS) $(CPPFLAGS) -DCASE=$$n -fsyntax-only bad_tables.cpp > $(BUILD)/bad.log 2>&1; then \
	    echo "bad-tables: case $$n compiled but shouldn't have"; exit 1; \
	  elif ! grep -q "$$want" $(BUILD)/bad.log; then \
	    echo "bad-tables: case $$n failed without naming $$want:"; cat $(BUILD)/bad.log; exit 1; \
	  fi; \
	done; echo "bad-tables: ok"

test: $(BUILD)/test_values $(BUILD)/sim_device | $(BUILD)
	$(BUILD)/test_values
	$(MAKE) --no-print-directory bad-tables
	python3 host_test.py $(BUILD)/sim_device

clean:
	rm -rf $(BUILD)
