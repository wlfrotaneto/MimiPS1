.PHONY: all build recompiler runtime dmg clean test

BUILD_DIR ?= build
DIST_DIR ?= dist

all: build

build:
	@mkdir -p $(BUILD_DIR)
	@cmake -B $(BUILD_DIR) -S .
	@cmake --build $(BUILD_DIR) -j

recompiler:
	@mkdir -p $(BUILD_DIR)
	@cmake -B $(BUILD_DIR) -S .
	@cmake --build $(BUILD_DIR) --target mimips1-recomp -j

dmg: build
	@cmake --build $(BUILD_DIR) --target dmg

clean:
	@rm -rf $(BUILD_DIR) $(DIST_DIR) recompiled
	@echo "Cleaned build artifacts."

help:
	@echo "MimiPS1-Recomp Build System"
	@echo "  make              - Build recompiler CLI and native runtime"
	@echo "  make dmg          - Build and package macOS Apple Silicon DMG"
	@echo "  make clean        - Remove build artifacts"
