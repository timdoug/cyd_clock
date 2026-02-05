# ESP-IDF project Makefile
# Convenience wrapper around idf.py

# Auto-detect serial port (macOS)
PORT ?= $(firstword $(wildcard /dev/cu.usbserial-* /dev/cu.wchusbserial*))

.PHONY: all build flash monitor clean fullclean menuconfig setup help

all: build

build:
	idf.py build

flash: build
	@if [ -z "$(PORT)" ]; then echo "Error: No serial port found. Set PORT=/dev/..."; exit 1; fi
	idf.py -p $(PORT) flash

monitor:
	@if [ -z "$(PORT)" ]; then echo "Error: No serial port found. Set PORT=/dev/..."; exit 1; fi
	idf.py -p $(PORT) monitor

# Build, flash, and open serial monitor
run: build
	@if [ -z "$(PORT)" ]; then echo "Error: No serial port found. Set PORT=/dev/..."; exit 1; fi
	idf.py -p $(PORT) flash monitor

clean:
	idf.py clean

fullclean:
	idf.py fullclean

menuconfig:
	idf.py menuconfig

# First-time setup after fresh checkout
setup:
	idf.py set-target esp32

help:
	@echo "Usage: make [target]"
	@echo ""
	@echo "Targets:"
	@echo "  setup      - First-time setup (run after fresh checkout)"
	@echo "  build      - Build the project"
	@echo "  flash      - Build and flash to device"
	@echo "  monitor    - Open serial monitor"
	@echo "  run        - Build, flash, and monitor (most common)"
	@echo "  clean      - Clean build artifacts"
	@echo "  fullclean  - Full clean (removes sdkconfig)"
	@echo "  menuconfig - Open ESP-IDF configuration menu"
	@echo ""
	@echo "Serial port: $(or $(PORT),<not found>)"
	@echo "Override with: make flash PORT=/dev/cu.usbserial-XXX"
