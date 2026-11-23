# Visual Gridrouter: core tests and the macOS app.
#
# Works from this folder or from the repository root as `make -C viewer ...`.
# Needs only the Command Line Tools (clang, swift, make, plutil, codesign).
#
#   make test      build and run the core tests, and the C99 header smoke test
#   make asan      the same under AddressSanitizer + UndefinedBehaviorSanitizer
#   make dump      build/rvdump (trace inspector) and build/rvsynth (synthetic test trace writer)
#   make app       swift build -c release, then wrap it into build/Visual Gridrouter.app
#   make selftest  build the app and run its binary with --selftest (exits by itself)
#   make run       open the app bundle
#   make clean     remove build/ and .build/

ifeq ($(origin CXX),default)
CXX = clang++
endif
ifeq ($(origin CC),default)
CC = cc
endif
SWIFT    ?= swift

BUILD    := build
CORE_INC := core/include
# Globs, not lists: new core files are picked up here and by SwiftPM alike.
CORE_SRC := $(wildcard core/src/*.cpp)
CORE_HDR := $(wildcard core/include/*.h core/src/*.hpp)
TEST_SRC := $(wildcard core/tests/*.cpp)
TEST_HDR := $(wildcard core/tests/*.hpp)
SMOKE    := core/tests/c_abi_smoke.c

WARN     := -Wall -Wextra -Wpedantic -Werror
CXXFLAGS_RV := -std=c++17 $(WARN) -I $(CORE_INC) -I core/src -g -O1
CFLAGS_RV   := -std=c99 $(WARN) -I $(CORE_INC) -g -O1
SAN      := -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer

APP_NAME := Visual Gridrouter
APP      := $(BUILD)/$(APP_NAME).app
EXE      := RouteViewer
SWIFT_BUILD := $(SWIFT) build -c release -Xswiftc -warnings-as-errors

.PHONY: test asan dump app selftest run clean

# --- core tests --------------------------------------------------------------

$(BUILD)/rv_tests: $(CORE_SRC) $(TEST_SRC) $(CORE_HDR) $(TEST_HDR)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS_RV) -o $@ $(CORE_SRC) $(TEST_SRC)

$(BUILD)/c_abi_smoke: $(SMOKE) $(CORE_SRC) $(CORE_HDR)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS_RV) -c -o $(BUILD)/c_abi_smoke.o $(SMOKE)
	$(CXX) $(CXXFLAGS_RV) -o $@ $(BUILD)/c_abi_smoke.o $(CORE_SRC)

test: $(BUILD)/rv_tests $(BUILD)/c_abi_smoke
	./$(BUILD)/rv_tests
	./$(BUILD)/c_abi_smoke

$(BUILD)/asan/rv_tests: $(CORE_SRC) $(TEST_SRC) $(CORE_HDR) $(TEST_HDR)
	@mkdir -p $(BUILD)/asan
	$(CXX) $(CXXFLAGS_RV) $(SAN) -o $@ $(CORE_SRC) $(TEST_SRC)

$(BUILD)/asan/c_abi_smoke: $(SMOKE) $(CORE_SRC) $(CORE_HDR)
	@mkdir -p $(BUILD)/asan
	$(CC) $(CFLAGS_RV) $(SAN) -c -o $(BUILD)/asan/c_abi_smoke.o $(SMOKE)
	$(CXX) $(CXXFLAGS_RV) $(SAN) -o $@ $(BUILD)/asan/c_abi_smoke.o $(CORE_SRC)

asan: $(BUILD)/asan/rv_tests $(BUILD)/asan/c_abi_smoke
	./$(BUILD)/asan/rv_tests
	./$(BUILD)/asan/c_abi_smoke

# --- command-line tools ---------------------------------------------------------

# rvdump uses only rv.h, like a client.  rvsynth writes the synthetic test
# trace of core/tests/synth.cpp, for trying rvdump without the recorder.
$(BUILD)/rvdump: tools/rvdump.cpp $(CORE_SRC) $(CORE_HDR)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS_RV) -o $@ tools/rvdump.cpp $(CORE_SRC)

$(BUILD)/rvsynth: tools/rvsynth.cpp core/tests/synth.cpp core/tests/synth.hpp
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS_RV) -I core/tests -o $@ tools/rvsynth.cpp core/tests/synth.cpp

dump: $(BUILD)/rvdump $(BUILD)/rvsynth

# --- macOS app ---------------------------------------------------------------

# Always delegated to SwiftPM, which rebuilds incrementally.
app:
	$(SWIFT_BUILD)
	sh mac/bundle.sh

selftest: app
	"./$(APP)/Contents/MacOS/$(EXE)" --selftest

run:
	open "$(APP)"

clean:
	rm -rf $(BUILD) .build
