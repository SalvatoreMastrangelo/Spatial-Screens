SDK_DIR := ../sdk
SDK_LIB := $(SDK_DIR)/lib/x86_64

CXX ?= g++
CXXFLAGS := -O2 -std=c++17 -Wall -Wextra -I$(SDK_DIR)/include -I../bridge
VK_LIBS := -lvulkan -lX11 -lXext -lXrandr -lXfixes

CAP_PC := dbus-1 libpipewire-0.3
CAP_CFLAGS := $(shell pkg-config --cflags $(CAP_PC))
CAP_LIBS := $(shell pkg-config --libs $(CAP_PC))
CXXFLAGS += $(CAP_CFLAGS)

LDFLAGS := -L$(SDK_LIB) -lglasses $(VK_LIBS) -lpthread $(CAP_LIBS) \
	-Wl,--disable-new-dtags,-rpath,'$$ORIGIN/$(SDK_LIB)' \
	-Wl,--allow-shlib-undefined

SHADER_HDRS := shaders/quad.vert.spv.h shaders/quad.frag.spv.h
OBJS := src/main.o src/vk_renderer.o src/vk_surface.o src/gesture_client.o \
        src/gesture_parse.o \
        src/capture.o src/capture_xshm.o src/config.o src/capture_portal.o src/telemetry.o

spatial-screens: $(OBJS)
	$(CXX) $(OBJS) -o $@ $(LDFLAGS)

# gesture_client / gesture_parse don't touch Vulkan or the shaders — lighter prereqs.
src/gesture_client.o: src/gesture_client.cpp src/gesture_client.h src/gesture_parse.h
	$(CXX) $(CXXFLAGS) -c $< -o $@

src/gesture_parse.o: src/gesture_parse.cpp src/gesture_parse.h src/gesture_client.h
	$(CXX) $(CXXFLAGS) -c $< -o $@

src/gesture_parse_test.o: src/gesture_parse_test.cpp src/gesture_parse.h src/gesture_client.h
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Pure-logic parser test — links only the parser unit, no SDK/Vulkan/socket deps.
gesture-parse-test: src/gesture_parse_test.o src/gesture_parse.o
	$(CXX) $^ -o $@

test: gesture-parse-test
	./gesture-parse-test

src/config.o: src/config.cpp src/config.h
	$(CXX) $(CXXFLAGS) -c $< -o $@

src/telemetry.o: src/telemetry.cpp src/telemetry.h ../bridge/ws_server.hpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

src/main.o: src/gesture_client.h src/capture.h src/config.h src/telemetry.h src/capture_portal.h

src/%.o: src/%.cpp $(SHADER_HDRS) src/vk_renderer.h src/vk_surface.h
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Generated SPIR-V headers are checked in so builds work without glslang.
shaders/%.vert.spv.h: shaders/%.vert
	glslangValidator -V --vn $(subst .,_,$(notdir $<))_spv -o $@ $<
shaders/%.frag.spv.h: shaders/%.frag
	glslangValidator -V --vn $(subst .,_,$(notdir $<))_spv -o $@ $<

vk-test: src/vk_test.o src/vk_renderer.o src/vk_surface.o
	$(CXX) $^ -o $@ $(VK_LIBS)

# Throwaway hardware spike: does SBS/3D mode switching work on Linux?
# SDK-only (no Vulkan/capture). Run via ./run-spike.sh. See
# docs/testing/2026-07-05-sbs-3d-spike-handoff.md.
src/sbs_spike.o: src/sbs_spike.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@
sbs-spike: src/sbs_spike.o
	$(CXX) $^ -o $@ -L$(SDK_LIB) -lglasses -lpthread \
		-Wl,--disable-new-dtags,-rpath,'$$ORIGIN/$(SDK_LIB)' \
		-Wl,--allow-shlib-undefined

CAPTURE_OBJS := src/capture.o src/capture_xshm.o src/capture_portal.o

# Capture objects don't touch Vulkan or the shaders — lighter prereqs.
src/capture.o src/capture_xshm.o src/capture_test.o: src/capture.h src/vk_surface.h

src/capture_portal.o: src/capture_portal.cpp src/capture_portal.h src/capture.h
	$(CXX) $(CXXFLAGS) -c $< -o $@
src/capture_test.o: src/capture_portal.h

capture-test: src/capture_test.o $(CAPTURE_OBJS) src/vk_surface.o
	$(CXX) $^ -o $@ $(VK_LIBS) $(CAP_LIBS) -lpthread

run: spatial-screens
	./run.sh
clean:
	rm -f spatial-screens vk-test capture-test gesture-parse-test sbs-spike src/*.o

.PHONY: run clean test
