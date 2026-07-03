SDK_DIR := ../sdk
SDK_LIB := $(SDK_DIR)/lib/x86_64

CXX ?= g++
CXXFLAGS := -O2 -std=c++17 -Wall -Wextra -I$(SDK_DIR)/include
LDFLAGS := -L$(SDK_LIB) -lglasses -lGL -lX11 -lXext -lXrandr -lpthread \
	-Wl,--disable-new-dtags,-rpath,'$$ORIGIN/$(SDK_LIB)' \
	-Wl,--allow-shlib-undefined

spatial-screens: src/main.cpp
	$(CXX) $(CXXFLAGS) src/main.cpp -o $@ $(LDFLAGS)

run: spatial-screens
	./run.sh

clean:
	rm -f spatial-screens

.PHONY: run clean
