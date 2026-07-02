# ESP-IDF project Makefile
# Convenience wrapper around idf.py

# Auto-detect serial port (macOS)
PORT ?= $(firstword $(wildcard /dev/cu.usbserial-* /dev/cu.wchusbserial*))
BAUD ?= 2000000
TZDATA_VERSION ?=

# Board profile, selected from main/boards/<name>.h. Override with the
# CYD_BOARD env var or `make CYD_BOARD=esp32_2432s024 ...`.
CYD_BOARD ?= esp32_2432s028

IDF = idf.py -D CYD_BOARD=$(CYD_BOARD)
IDF_PORT = $(IDF) -p $(PORT)
IDF_FLASH = $(IDF_PORT) -b $(BAUD)

define check-port
	@if [ -z "$(PORT)" ]; then echo "Error: No serial port found. Set PORT=/dev/..."; exit 1; fi
endef

.PHONY: all build flash app-flash monitor run app-run clean fullclean menuconfig setup update-tzdata help

all: build
build:                ; $(IDF) build
flash:                ; $(check-port) && $(IDF_FLASH) flash
app-flash:            ; $(check-port) && $(IDF_FLASH) app-flash
monitor:              ; $(check-port) && $(IDF_PORT) monitor
run:                  ; $(check-port) && $(IDF_FLASH) flash && $(IDF_PORT) monitor
app-run:              ; $(check-port) && $(IDF_FLASH) app-flash && $(IDF_PORT) monitor
clean:                ; $(IDF) clean
fullclean:            ; $(IDF) fullclean && rm -rf build sdkconfig
menuconfig:           ; $(IDF) menuconfig
setup:                ; $(IDF) set-target esp32
update-tzdata:        ; python3 tzdata/extract_posix.py $(if $(TZDATA_VERSION),--download $(TZDATA_VERSION),--download) --update-ui main/ui_timezone.c

help:
	@echo "Usage: make [target]"
	@echo ""
	@echo "Targets:"
	@echo "  setup      - First-time setup (run after fresh checkout)"
	@echo "  build      - Build the project"
	@echo "  flash      - Build and flash to device"
	@echo "  app-flash  - Flash app only (faster, skips bootloader)"
	@echo "  monitor    - Open serial monitor"
	@echo "  run        - Build, flash, and monitor (most common)"
	@echo "  app-run    - Build, flash app only, and monitor (fastest)"
	@echo "  clean      - Clean build artifacts"
	@echo "  fullclean  - Full clean (removes sdkconfig)"
	@echo "  menuconfig - Open ESP-IDF configuration menu"
	@echo "  update-tzdata - Download tzdata and update timezone table"
	@echo ""
	@echo "Serial port: $(or $(PORT),<not found>)"
	@echo "Override with: make flash PORT=/dev/cu.usbserial-XXX"
	@echo "Board: $(CYD_BOARD) (override: make CYD_BOARD=esp32_2432s024 ...)"
	@echo "Pin tzdata: make update-tzdata TZDATA_VERSION=2026c"
