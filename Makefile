CXX ?= g++
TARGET := sort_stf_by_femid
SRC := sort_stf_by_femid.cc
CXXFLAGS ?= -O3 -DNDEBUG -std=c++17 -Wall -Wextra -Wpedantic

.PHONY: all clean help

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) $(SRC) -o $(TARGET)

clean:
	rm -f $(TARGET)

help:
	@echo "make        Build $(TARGET)"
	@echo "make clean  Remove the executable"
	@echo "make help   Show this help"
