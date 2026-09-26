# Native Haiku build with the development tools bundled with Haiku; the core
# (src/core, no Haiku API) also builds and tests on Linux:
#   make BUILD=build-host check-host
.DEFAULT_GOAL := all
CXX ?= g++
BUILD ?= build-haiku
CPPFLAGS += -Isrc
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++17 -Wall -Wextra -Wno-multichar -Wno-unused-parameter -Wno-sign-compare
CORE = $(wildcard src/core/*.cpp)
UI = $(wildcard src/ui/*.cpp) src/main.cpp
CORE_OBJ = $(CORE:%.cpp=$(BUILD)/%.o)
UI_OBJ = $(UI:%.cpp=$(BUILD)/%.o)
ifeq ($(shell uname),Haiku)
CORE_LIBS = -lnetwork
else
CORE_LIBS = -lpthread
endif
LIBS = -lbe -ltracker $(CORE_LIBS)
.PHONY: all core openvpn package clean icon check check-host
all: $(BUILD)/Burrow
core: $(CORE_OBJ)
$(BUILD)/Burrow: $(CORE_OBJ) $(UI_OBJ) resources/Burrow.rdef resources/branding/burrow-icon.hvif
	$(CXX) -o $@.new $(CORE_OBJ) $(UI_OBJ) $(LIBS)
	rc -o $(BUILD)/Burrow.rsrc resources/Burrow.rdef
	xres -o $@.new $(BUILD)/Burrow.rsrc
	mimeset -f $@.new
	mv $@.new $@
$(BUILD)/%.o: %.cpp
	mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@
$(BUILD)/burrow_core_tests: $(CORE_OBJ) $(BUILD)/tests/CoreTests.o
	$(CXX) -o $@ $^ $(CORE_LIBS)
check: $(BUILD)/burrow_core_tests
	$(BUILD)/burrow_core_tests tests/fake-openvpn.py
check-host: check
openvpn:
	sh openvpn/build.sh
icon:
	python3 tools/make-icon.py resources/branding/burrow-icon.hvif resources/branding/burrow-icon-preview.png
package: all
	bash tools/package-haiku.sh
clean:
	rm -rf $(BUILD)
-include $(CORE_OBJ:.o=.d) $(UI_OBJ:.o=.d) $(BUILD)/tests/CoreTests.d
