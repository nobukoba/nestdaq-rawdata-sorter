CXX ?= g++
TARGETS := sort_stf_by_femid extract_first_seconds
CXXFLAGS ?= -O3 -DNDEBUG -std=c++17 -Wall -Wextra -Wpedantic

.PHONY: all clean help

all: $(TARGETS)

sort_stf_by_femid: sort_stf_by_femid.cc
	$(CXX) $(CXXFLAGS) $< -o $@

extract_first_seconds: extract_first_seconds.cc
	$(CXX) $(CXXFLAGS) $< -o $@

clean:
	rm -f $(TARGETS)

help:
	@echo "make        Build all utilities"
	@echo "make clean  Remove executables"
	@echo "make help   Show this help"
