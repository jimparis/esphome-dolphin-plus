import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import CONF_ID

from . import DolphinBle

CONF_DOLPHIN_BLE_ID = "dolphin_ble_id"
CONF_KIND = "kind"

KINDS = {
    "filter_state": 0,
    "is_smart": 1,
    "cycle_duration": 2,
    "cycle_time_remaining": 3,
    "temperature": 4,
    "robot_type": 5,
    "turn_on_count": 6,
    "mu_flash_write_counter": 7,
    "mu_pcb_runtime": 8,
    "mu_impeller_runtime": 9,
    "mu_not_completed_cycles": 10,
    "mu_climb_period": 11,
    "sm_timezone": 12,
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
