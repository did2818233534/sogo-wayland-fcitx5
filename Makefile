CXX ?= g++
PKGS = Fcitx5Core gio-2.0 libcurl
CPPFLAGS += $(shell pkg-config --cflags $(PKGS))
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++17 -fPIC -pthread
LDLIBS += $(shell pkg-config --libs $(PKGS)) -pthread

.PHONY: all check clean
all: build/libsogou-native.so
build/libsogou-native.so: src/sogou-native.cpp
	@mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -shared $< -o $@ $(LDFLAGS) $(LDLIBS)
check:
	python3 -m unittest discover -s tests -v
clean:
	rm -rf build
