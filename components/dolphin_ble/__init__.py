import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import esp32_ble_tracker
from esphome.components import time as time_
from esphome.const import CONF_ID, CONF_MAC_ADDRESS

CODEOWNERS = ["@local"]
DEPENDENCIES = ["esp32_ble_tracker"]

CONF_TIME_ID = "time_id"
CONF_TIME_IDS = "time_ids"
CONF_TEMPERATURE_SUPPORTED = "temperature_supported"

dolphin_ble_ns = cg.esphome_ns.namespace("dolphin_ble")
DolphinBle = dolphin_ble_ns.class_("DolphinBle", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(DolphinBle),
        cv.Optional(CONF_MAC_ADDRESS, default=""): cv.Any(
            cv.mac_address, cv.one_of("", lower=True)
        ),
        cv.Optional(CONF_TIME_ID): cv.use_id(time_.RealTimeClock),
        cv.Optional(CONF_TIME_IDS): cv.ensure_list(cv.use_id(time_.RealTimeClock)),
        cv.Optional(CONF_TEMPERATURE_SUPPORTED, default=False): cv.boolean,
    }
).extend(cv.COMPONENT_SCHEMA).extend(esp32_ble_tracker.ESP_BLE_DEVICE_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_mac_address(str(config[CONF_MAC_ADDRESS])))
    cg.add(var.set_temperature_supported(config[CONF_TEMPERATURE_SUPPORTED]))
    await esp32_ble_tracker.register_ble_device(var, config)
    if CONF_TIME_ID in config:
        time_var = await cg.get_variable(config[CONF_TIME_ID])
        cg.add(var.set_time_id(time_var))
    for time_id in config.get(CONF_TIME_IDS, []):
        time_var = await cg.get_variable(time_id)
        cg.add(var.add_time_id(time_var))
