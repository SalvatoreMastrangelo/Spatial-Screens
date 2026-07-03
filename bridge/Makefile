SDK_DIR := ../sdk
SDK_LIB := $(SDK_DIR)/lib/x86_64

CXX ?= g++
CXXFLAGS := -O2 -std=c++17 -Wall -Wextra -I$(SDK_DIR)/include
# --disable-new-dtags forces DT_RPATH, which (unlike DT_RUNPATH) also resolves
# libcarina_vio.so's transitive OpenCV deps from the SDK directory.
# --allow-shlib-undefined: libcarina_vio.so's OpenCV symbols resolve at runtime
# from the bundled libs in SDK_LIB (via rpath); no need to link them here.
LDFLAGS := -L$(SDK_LIB) -lglasses -lpthread \
	-Wl,--disable-new-dtags,-rpath,'$$ORIGIN/$(SDK_LIB)' \
	-Wl,--allow-shlib-undefined

viture-bridge: main.cpp ws_server.hpp
	$(CXX) $(CXXFLAGS) main.cpp -o $@ $(LDFLAGS)

run: viture-bridge
	./viture-bridge

install-udev:
	sudo cp 70-viture-xr.rules /etc/udev/rules.d/
	sudo udevadm control --reload-rules && sudo udevadm trigger

clean:
	rm -f viture-bridge

.PHONY: run install-udev clean
