CXX=gcc
CPPFLAGS=-lstdc++ -lfabric -O3

SOURCES := $(shell ls *.cpp)

APPS := $(shell echo libfabric_helloworld)

.PHONY: all
all: $(APPS)

#default rule for all apps
% : %.cpp
	$(CXX) $(CPPFLAGS) $^ -o $@

.PHONY: clean
clean:
	rm -f $(APPS)
