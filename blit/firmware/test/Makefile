# Native tests for the blit firmware library. Needs g++ (C++20) and, for the
# end-to-end test, Node 22 or later.
#
#   make            build and run everything
#   make sim        just the simulated display (build/sim_display)

CXX ?= g++
CXXFLAGS ?= -std=c++20 -O1 -g -Wall -Wextra -Wno-unused-parameter -Werror
CPPFLAGS += -I../src
LDLIBS += -lpthread

LIB_SRCS := ../src/blit/Wire.cpp ../src/blit/Frame.cpp ../src/blit/Receiver.cpp ../src/blit/Bmp.cpp
LIB_HDRS := ../src/blit.h $(wildcard ../src/blit/*.h)
BUILD := build

.PHONY: all test sim clean
all: test

$(BUILD):
	mkdir -p $@

$(BUILD)/test_receiver: test_receiver.cpp $(LIB_SRCS) $(LIB_HDRS) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ test_receiver.cpp $(LIB_SRCS) $(LDLIBS)

$(BUILD)/sim_display: sim_display.cpp $(LIB_SRCS) $(LIB_HDRS) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ sim_display.cpp $(LIB_SRCS) $(LDLIBS)

sim: $(BUILD)/sim_display

test: $(BUILD)/test_receiver $(BUILD)/sim_display
	$(BUILD)/test_receiver
	SIM_DISPLAY=$(BUILD)/sim_display node --test host.test.mjs

clean:
	rm -rf $(BUILD)
