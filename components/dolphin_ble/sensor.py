import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import CONF_ID

from . import DolphinBle

CONF_DOLPHIN_BLE_ID = "dolphin_ble_id"
CONF_KIND = "kind"

KINDS = {
    "filter_state": 1,
    "is_smart": 2,
    "cycle_time": 3,
    "start_cycle_time": 4,
    "cycle_start_utc": 5,
    "temperature": 6,
    "temperature_timestamp": 7,
    "measuring": 8,
    "reading_during_cycle": 9,
    "robot_type": 10,
    "turn_on_count": 11,
    "mu_sw_version_major": 12,
    "mu_sw_version_minor": 13,
    "mu_flash_write_counter": 14,
    "mu_cycle_time": 15,
}

CONFIG_SCHEMA = sensor.sensor_schema().extend(
    {
        cv.Required(CONF_DOLPHIN_BLE_ID): cv.use_id(DolphinBle),
        cv.Required(CONF_KIND): cv.enum(KINDS, lower=True, space="_"),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_DOLPHIN_BLE_ID])
    var = await sensor.new_sensor(config)
    cg.add(parent.set_numeric_sensor(config[CONF_KIND], var))
