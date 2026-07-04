SDK_DIR := ../sdk
SDK_LIB := $(SDK_DIR)/lib/x86_64

CXX ?= g++
CXXFLAGS := -O2 -std=c++17 -Wall -Wextra -I$(SDK_DIR)/include
VK_LIBS := -lvulkan -lX11 -lXext -lXrandr
LDFLAGS := -L$(SDK_LIB) -lglasses $(VK_LIBS) -lpthread \
	-Wl,--disable-new-dtags,-rpath,'$$ORIGIN/$(SDK_LIB)' \
	-Wl,--allow-shlib-undefined

SHADER_HDRS := shaders/quad.vert.spv.h shaders/quad.frag.spv.h
OBJS := src/main.o src/vk_renderer.o src/vk_surface.o

spatial-screens: $(OBJS)
	$(CXX) $(OBJS) -o $@ $(LDFLAGS)

src/%.o: src/%.cpp $(SHADER_HDRS) src/vk_renderer.h src/vk_surface.h
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Generated SPIR-V headers are checked in so builds work without glslang.
shaders/%.vert.spv.h: shaders/%.vert
	glslangValidator -V --vn $(subst .,_,$(notdir $<))_spv -o $@ $<
shaders/%.frag.spv.h: shaders/%.frag
	glslangValidator -V --vn $(subst .,_,$(notdir $<))_spv -o $@ $<

vk-test: src/vk_test.o src/vk_renderer.o src/vk_surface.o
	$(CXX) $^ -o $@ $(VK_LIBS)

run: spatial-screens
	./run.sh
clean:
	rm -f spatial-screens vk-test src/*.o

.PHONY: run clean
