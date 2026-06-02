# =============================================================================
# BThomeV2-nRF52-Zephyr — convenience Makefile
#
# Usage:
#   make <NNN>-build   Build sample NNN for the default board
#   make <NNN>-flash   Build (if needed) and flash sample NNN
#   make <NNN>-clean   Remove the build directory for sample NNN
#   make all-build     Build all samples
#   make help          Show this help
#
# Override the target board:
#   make 000-build BOARD=xiao_ble
#
# The build directory is always build/ (west default) and is reused between
# successive builds of the same sample.  Switch samples by using the
# -p always (pristine) flag, which west handles for us via WEST_BUILD_PRISTINE.
# =============================================================================

BOARD         ?= nrf52840dk_nrf52840
MONITOR_BAUD  ?= 115200

# ---------------------------------------------------------------------------
# USB VID:PID table — used by scripts/find-port.sh for auto-detection
# ---------------------------------------------------------------------------
# Monitor port: USB-CDC UART of the running firmware / J-Link bridge
_MONITOR_VIDPID_nrf52840dk_nrf52840 := 1915:c00a
_MONITOR_PRODUCT_nrf52840dk_nrf52840 := Connectivity   # selects UART bridge, not PPK2
_MONITOR_VIDPID_xiao_ble             := 2fe3:0100
_MONITOR_VIDPID_xiao_ble_sense       := 2fe3:0100

# DFU port: bootloader CDC port (double-tap RESET to activate)
_DFU_VIDPID_xiao_ble                 := 2886:0045
_DFU_VIDPID_xiao_ble_sense           := 2886:0046

# Auto-detect MONITOR_PORT based on board VID:PID (fallback: /dev/ttyACM0)
MONITOR_PORT ?= $(or \
    $(shell scripts/find-port.sh --quiet \
        $(if $(_MONITOR_PRODUCT_$(BOARD)),--product "$(_MONITOR_PRODUCT_$(BOARD))") \
        "$(_MONITOR_VIDPID_$(BOARD))" 2>/dev/null), \
    /dev/ttyACM0)

# Auto-detect XIAO_DFU_PORT based on board DFU VID:PID (fallback: /dev/ttyACM0)
XIAO_DFU_PORT ?= $(or \
    $(shell scripts/find-port.sh --quiet \
        "$(_DFU_VIDPID_$(BOARD))" 2>/dev/null), \
    /dev/ttyACM0)

# Fixed serial port for -serial-flash targets (skips auto-detection).
# Override on the command line: make 099-sysoff-serial-flash SERIAL_PORT=/dev/ttyACM1
SERIAL_PORT ?= /dev/ttyACM3

# If SERIAL_PORT was explicitly passed on the command line, use it as the DFU
# port directly (skips VID:PID auto-detection).  Otherwise fall back to the
# auto-detected XIAO_DFU_PORT value.
_FLASH_PORT = $(if $(filter command line,$(origin SERIAL_PORT)),$(SERIAL_PORT),$(XIAO_DFU_PORT))

WEST   := west
BUILD  := build

# Boards that use the Adafruit UF2 bootloader instead of J-Link/nrfjprog.
# west flash won't work for these; we copy the .uf2 file instead.
XIAO_BOARDS := xiao_ble xiao_ble_sense

# Returns non-empty string when $(BOARD) is in XIAO_BOARDS.
_IS_XIAO := $(filter $(BOARD),$(XIAO_BOARDS))

# ---------------------------------------------------------------------------
# Sample definitions
# ---------------------------------------------------------------------------
SAMPLE_000 := samples/000_blinky
SAMPLE_010 := samples/010_bthome-tut1
SAMPLE_020 := samples/020_bthome_tut2
SAMPLE_098 := samples/098_test_32khz
SAMPLE_099 := samples/099_xiao_power_tests
SAMPLE_100 := samples/100_bthome_pir
SAMPLE_200 := samples/200_bthome_imu

# ---------------------------------------------------------------------------
# Internal helpers
# ---------------------------------------------------------------------------

# build_sample: always starts a pristine build (-p always).
# Use this for NNN-build targets where a clean rebuild is expected.
define build_sample
	$(WEST) build -p always -b $(BOARD) $(1)
endef

# flash_sample: incremental build (no -p always) + flash.
# west build detects whether sources changed; skips compilation if up-to-date.
# If the board changes, west will detect the mismatch and abort with a hint to
# run make NNN-build first (or pass -p always manually).
# On XIAO boards: pass SERIAL_PORT=/dev/ttyACMx on the command line to use that
# port directly instead of VID:PID auto-detection.
define flash_sample
	$(WEST) build -b $(BOARD) $(1)
	$(if $(_IS_XIAO), \
		$(MAKE) _uf2_copy XIAO_DFU_PORT=$(_FLASH_PORT), \
		$(WEST) flash \
	)
endef

# serial_flash_sample: pristine build + flash via SERIAL_PORT directly.
# Uses -p always so switching between samples never hits a stale build dir.
# Use for NNN-serial-flash targets or when auto-detection is unreliable.
define serial_flash_sample
	$(WEST) build -p always -b $(BOARD) $(1)
	$(MAKE) _uf2_copy XIAO_DFU_PORT=$(SERIAL_PORT)
endef

# Flash XIAO via adafruit-nrfutil DFU over USB serial.
# Requires: uv tool install adafruit-nrfutil
# Workflow: double-tap RESET → LED fades → board enters DFU bootloader.
.PHONY: _uf2_copy
_uf2_copy:
	@test -c "$(XIAO_DFU_PORT)" || \
		{ echo ""; \
		  echo "ERROR: DFU port $(XIAO_DFU_PORT) not found."; \
		  echo "  1. Double-tap the RESET button on the XIAO board"; \
		  echo "     until the LED starts fading in/out."; \
		  echo "  2. Retry: make <NNN>-flash BOARD=$(BOARD)"; \
		  echo "  Or specify port: make <NNN>-flash BOARD=$(BOARD) XIAO_DFU_PORT=/dev/ttyACM1"; \
		  echo ""; \
		  exit 1; }
	@echo "Creating DFU package from $(BUILD)/zephyr/zephyr.hex ..."
	adafruit-nrfutil dfu genpkg \
		--dev-type 0x0052 \
		--application $(BUILD)/zephyr/zephyr.hex \
		$(BUILD)/dfu_package.zip
	@echo "Flashing via DFU on $(XIAO_DFU_PORT) ..."
	adafruit-nrfutil dfu serial \
		--package $(BUILD)/dfu_package.zip \
		--port $(XIAO_DFU_PORT) \
		--baudrate 115200 \
		--touch 1200
	@echo "Done."

# ---------------------------------------------------------------------------
# 000 — blinky
# ---------------------------------------------------------------------------
.PHONY: 000-build
000-build:
	$(call build_sample,$(SAMPLE_000))

.PHONY: 000-flash
000-flash:
	$(call flash_sample,$(SAMPLE_000))

.PHONY: 000-serial-flash
000-serial-flash:
	$(call serial_flash_sample,$(SAMPLE_000))

.PHONY: 000-clean
000-clean:
	rm -rf $(BUILD)

# ---------------------------------------------------------------------------
# 010 — bthome-tut1
# ---------------------------------------------------------------------------
.PHONY: 010-build
010-build:
	$(call build_sample,$(SAMPLE_010))

.PHONY: 010-flash
010-flash:
	$(call flash_sample,$(SAMPLE_010))

.PHONY: 010-serial-flash
010-serial-flash:
	$(call serial_flash_sample,$(SAMPLE_010))

.PHONY: 010-clean
010-clean:
	rm -rf $(BUILD)

# ---------------------------------------------------------------------------
# 020 — bthome_tut2 (power-managed)
# ---------------------------------------------------------------------------
.PHONY: 020-build
020-build:
	$(call build_sample,$(SAMPLE_020))

.PHONY: 020-flash
020-flash:
	$(call flash_sample,$(SAMPLE_020))

.PHONY: 020-serial-flash
020-serial-flash:
	$(call serial_flash_sample,$(SAMPLE_020))

.PHONY: 020-clean
020-clean:
	rm -rf $(BUILD)

# ---------------------------------------------------------------------------
# 098 — test_32khz (32 kHz clock source diagnostic)
# ---------------------------------------------------------------------------
.PHONY: 098-build
098-build:
	$(call build_sample,$(SAMPLE_098))

.PHONY: 098-flash
098-flash:
	$(call flash_sample,$(SAMPLE_098))

.PHONY: 098-serial-flash
098-serial-flash:
	$(call serial_flash_sample,$(SAMPLE_098))

.PHONY: 098-clean
098-clean:
	rm -rf $(BUILD)

# ---------------------------------------------------------------------------
# 099 — xiao_power_tests (idle-floor baseline)
# ---------------------------------------------------------------------------
.PHONY: 099-build
099-build:
	$(call build_sample,$(SAMPLE_099))

.PHONY: 099-flash
099-flash:
	$(call flash_sample,$(SAMPLE_099))

# Hardware-floor diagnostic build: calls sys_poweroff() instead of k_sleep().
# In System OFF, CPU + RAM (except retained) + all peripherals + all clocks
# are physically off.  Whatever current still flows is pure board hardware
# (LEDs, charger quiescent, external pull-ups, leakage).
.PHONY: 099-sysoff-build
099-sysoff-build:
	$(WEST) build -p always -b $(BOARD) $(SAMPLE_099) -- -DCONFIG_APP_SYSTEM_OFF=y

.PHONY: 099-sysoff-flash
099-sysoff-flash: 099-sysoff-build
	$(if $(_IS_XIAO),$(MAKE) _uf2_copy,$(WEST) flash)

# Flash via a fixed serial port — skips VID:PID auto-detection.
# Useful when the auto-detect fails or multiple XIAO boards are connected.
# Default port: /dev/ttyACM3  (override: SERIAL_PORT=/dev/ttyACM1)
.PHONY: 099-sysoff-serial-flash
099-sysoff-serial-flash: 099-sysoff-build
	$(MAKE) _uf2_copy XIAO_DFU_PORT=$(SERIAL_PORT)

.PHONY: 099-serial-flash
099-serial-flash:
	$(call serial_flash_sample,$(SAMPLE_099))

.PHONY: 099-clean
099-clean:
	rm -rf $(BUILD)

# ---------------------------------------------------------------------------
# 100 — bthome_pir
# ---------------------------------------------------------------------------
.PHONY: 100-build
100-build:
	$(call build_sample,$(SAMPLE_100))

.PHONY: 100-flash
100-flash:
	$(call flash_sample,$(SAMPLE_100))

.PHONY: 100-serial-flash
100-serial-flash:
	$(call serial_flash_sample,$(SAMPLE_100))

.PHONY: 100-clean
100-clean:
	rm -rf $(BUILD)

# ---------------------------------------------------------------------------
# 200 — bthome_imu
# ---------------------------------------------------------------------------
.PHONY: 200-build
200-build:
	$(call build_sample,$(SAMPLE_200))

.PHONY: 200-flash
200-flash:
	$(call flash_sample,$(SAMPLE_200))

.PHONY: 200-serial-flash
200-serial-flash:
	$(call serial_flash_sample,$(SAMPLE_200))

.PHONY: 200-clean
200-clean:
	rm -rf $(BUILD)

# ---------------------------------------------------------------------------
# Aggregate targets
# ---------------------------------------------------------------------------
.PHONY: all-build
all-build: 000-build 010-build 020-build 099-build 100-build 200-build

.PHONY: clean
clean:
	rm -rf $(BUILD)

# ---------------------------------------------------------------------------
# Serial monitor (J-Link UART on nRF52840-DK)
# ---------------------------------------------------------------------------
.PHONY: monitor
monitor:
	picocom -b $(MONITOR_BAUD) --imap lfcrlf $(MONITOR_PORT)

# ---------------------------------------------------------------------------
# Serial port listing
# ---------------------------------------------------------------------------
.PHONY: list-serial list-serial-dfu list-serial-monitor
list-serial:
	@scripts/list-serial.sh

list-serial-dfu:
	@scripts/list-serial.sh --dfu

list-serial-monitor:
	@scripts/list-serial.sh --monitor

# ---------------------------------------------------------------------------
# Help
# ---------------------------------------------------------------------------
.PHONY: help
help:
	@echo ""
	@echo "BThomeV2-nRF52-Zephyr build targets"
	@echo "====================================="
	@echo ""
	@echo "  make 000-build           Pristine build of samples/000_blinky"
	@echo "  make 000-flash           Incremental build + flash (rebuilds only if changed)"
	@echo "  make 000-serial-flash    Incremental build + flash via SERIAL_PORT ($(SERIAL_PORT))"
	@echo "  make 000-clean           Remove build directory"
	@echo ""
	@echo "  make 010-build           Pristine build of samples/010_bthome-tut1"
	@echo "  make 010-flash           Incremental build + flash"
	@echo "  make 010-serial-flash    Incremental build + flash via SERIAL_PORT"
	@echo "  make 010-clean           Remove build directory"
	@echo ""
	@echo "  make 020-build           Pristine build of samples/020_bthome_tut2  (PM)"
	@echo "  make 020-flash           Incremental build + flash"
	@echo "  make 020-serial-flash    Incremental build + flash via SERIAL_PORT"
	@echo "  make 020-clean           Remove build directory"
	@echo ""
	@echo "  make 100-build           Pristine build of samples/100_bthome_pir"
	@echo "  make 100-flash           Incremental build + flash"
	@echo "  make 100-serial-flash    Incremental build + flash via SERIAL_PORT"
	@echo "  make 100-clean           Remove build directory"
	@echo ""
	@echo "  make all-build           Build all samples sequentially"
	@echo "  make clean               Remove build directory"
	@echo ""
	@echo "  make monitor             Open serial monitor on $(MONITOR_PORT) @ $(MONITOR_BAUD) baud"
	@echo "                           (override: MONITOR_PORT=/dev/ttyACM1)"
	@echo ""
	@echo "  make list-serial         List all ttyACM/ttyUSB ports with USB details"
	@echo "  make list-serial-dfu     Show only DFU/bootloader ports"
	@echo "  make list-serial-monitor Show only running-firmware ports"
	@echo ""
	@echo "  BOARD=$(BOARD)"
	@echo "   nRF52840-DK:  BOARD=nrf52840dk_nrf52840  (uses west flash / J-Link)"
	@echo "   XIAO plain:   BOARD=xiao_ble              (copies zephyr.uf2 via UF2)"
	@echo "   XIAO Sense:   BOARD=xiao_ble_sense        (copies zephyr.uf2 via UF2)"
	@echo ""
	@echo "  XIAO flash workflow (adafruit-nrfutil DFU):"
	@echo "   1. Double-tap RESET until LED fades → board enters DFU bootloader"
	@echo "   2. Board appears as CDC serial port"
	@echo "   3a. Auto-detect port:  make <NNN>-flash BOARD=xiao_ble_sense"
	@echo "   3b. Fixed port:        make <NNN>-flash BOARD=xiao_ble_sense SERIAL_PORT=/dev/ttyACM1"
	@echo "   3c. serial-flash:      make <NNN>-serial-flash BOARD=xiao_ble_sense"
	@echo "                          (always uses SERIAL_PORT=$(SERIAL_PORT))"
	@echo "   Install tool: uv tool install adafruit-nrfutil"
	@echo "   Current SERIAL_PORT:   $(SERIAL_PORT)"
	@echo "   Current XIAO_DFU_PORT: $(XIAO_DFU_PORT)  (VID:PID auto-detect)"
	@echo ""
