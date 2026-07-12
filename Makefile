ESPHOME ?= uv run esphome
CONFIG ?= dolphin_ble.yaml
PORT ?= /dev/ttyUSB0

.PHONY: help sync compile build flash upload ota logs logs-protocol run dashboard clean

help:
	@echo "Targets:"
	@echo "  make sync     Install/update ESPHome with uv"
	@echo "  make build    Compile $(CONFIG)"
	@echo "  make flash    Upload $(CONFIG) to $(PORT)"
	@echo "  make ota      Upload $(CONFIG) over the network"
	@echo "  make logs     Stream serial logs from $(PORT)"
	@echo "  make logs-protocol"
	@echo "                 Stream Dolphin protocol log lines from $(PORT)"
	@echo "  make run      Compile, upload, and stream logs"
	@echo "  make dashboard"
	@echo "                 Start the ESPHome dashboard in this repo"
	@echo "  make clean    Remove ESPHome build output"

sync:
	uv sync

compile build:
	$(ESPHOME) compile $(CONFIG)

flash upload:
	$(ESPHOME) upload $(CONFIG) --device $(PORT)

ota:
	$(ESPHOME) upload $(CONFIG)

logs:
	$(ESPHOME) logs $(CONFIG) --device $(PORT)

logs-protocol:
	$(ESPHOME) logs $(CONFIG) --device $(PORT) 2>&1 | rg --text 'Sending probe|Robot text frame|Robot notification len=|GATTC open|MTU configured|subscription'

run:
	$(ESPHOME) run $(CONFIG) --device $(PORT)

dashboard:
	$(ESPHOME) dashboard .

clean:
	rm -rf .esphome/build
