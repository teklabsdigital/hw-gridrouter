# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Trevor Attema, Teklabs Digital
#
# Visual Gridrouter: core tests and the macOS app.
#
# Works from this folder or from the repository root as `make -C viewer ...`.
# Needs only the Command Line Tools (clang, swift, make, plutil, codesign).
#
#   make test      build and run the core tests, the C99 header smoke test and the golden images
#   make asan      the core tests and smoke test under AddressSanitizer + UndefinedBehaviorSanitizer
#   make dump      build/rvdump (trace inspector) and build/rvsynth (synthetic test trace writer)
#   make render    build rvrender; draw the stress board, frames of the synthetic routing run and
#                  the recorder sample into build/render/
#   make golden    render the golden cases and compare them with mac/Tests/golden/*.png
#   make golden-update  replace mac/Tests/golden/*.png with fresh renders (review them first)
#   make app       swift build -c release, then wrap it into build/Visual Gridrouter.app
#   make selftest  build the app and run its binary with --selftest, without and with traces;
#                  then rvrender --selftest (playback, seeking, layers against one pass)
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
RVRENDER := .build/release/rvrender
# The recorder's sample (written by another tool; read, never written here).
SAMPLE   := core/tests/data/small.grtrace

.PHONY: test asan dump app selftest run clean rvrender render golden golden-update

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
	$(MAKE) golden

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

SYNTH_SRC := core/tests/synth.cpp core/tests/synth_board.cpp core/tests/synth_timeline.cpp
$(BUILD)/rvsynth: tools/rvsynth.cpp $(SYNTH_SRC) core/tests/synth.hpp
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS_RV) -I core/tests -o $@ tools/rvsynth.cpp $(SYNTH_SRC)

dump: $(BUILD)/rvdump $(BUILD)/rvsynth

# --- headless rendering and golden images --------------------------------------

# rvrender draws with the app's renderer (RVKit), so it is a SwiftPM product.
rvrender:
	$(SWIFT_BUILD) --product rvrender

render: rvrender $(BUILD)/rvsynth
	@mkdir -p $(BUILD)/render
	./$(BUILD)/rvsynth --stress $(BUILD)/render/stress.grtrace
	$(RVRENDER) $(BUILD)/render/stress.grtrace $(BUILD)/render/stress.png --size 1600x1150
	$(RVRENDER) $(BUILD)/render/stress.grtrace $(BUILD)/render/stress_closeup.png --size 1600x1000 --window 104,54,124,66.5
	./$(BUILD)/rvsynth --timeline $(BUILD)/render/timeline.grtrace
	$(RVRENDER) $(BUILD)/render/timeline.grtrace $(BUILD)/render/timeline --frames 12 --size 800x600
	@if [ -f $(SAMPLE) ]; then \
		$(RVRENDER) $(SAMPLE) $(BUILD)/render/small_final.png --size 1200x800 && \
		$(RVRENDER) $(SAMPLE) $(BUILD)/render/small_placed.png --size 1200x800 --board 0; \
	else echo "render: $(SAMPLE) not present, skipped"; fi

golden: rvrender $(BUILD)/rvsynth
	sh mac/Tests/golden.sh check

golden-update: rvrender $(BUILD)/rvsynth
	sh mac/Tests/golden.sh update

# --- macOS app ---------------------------------------------------------------

# Always delegated to SwiftPM, which rebuilds incrementally.
app:
	$(SWIFT_BUILD)
	sh mac/bundle.sh

# App runs: the welcome window; the recorder's sample (if present); the
# synthetic stress board; the synthetic routing run.  Each opens one window
# and exits by itself.  Then rvrender's playback self-test (no window) on
# the routing run and the sample.
selftest: app rvrender $(BUILD)/rvsynth
	"./$(APP)/Contents/MacOS/$(EXE)" --selftest
	@if [ -f $(SAMPLE) ]; then "./$(APP)/Contents/MacOS/$(EXE)" --selftest $(SAMPLE); \
	else echo "selftest: $(SAMPLE) not present, skipped"; fi
	./$(BUILD)/rvsynth --stress $(BUILD)/selftest-stress.grtrace
	"./$(APP)/Contents/MacOS/$(EXE)" --selftest $(BUILD)/selftest-stress.grtrace
	./$(BUILD)/rvsynth --timeline $(BUILD)/selftest-timeline.grtrace
	"./$(APP)/Contents/MacOS/$(EXE)" --selftest $(BUILD)/selftest-timeline.grtrace
	$(RVRENDER) --selftest $(BUILD)/selftest-timeline.grtrace
	@if [ -f $(SAMPLE) ]; then $(RVRENDER) --selftest $(SAMPLE); \
	else echo "selftest: $(SAMPLE) not present, skipped"; fi

run:
	open "$(APP)"

clean:
	rm -rf $(BUILD) .build
