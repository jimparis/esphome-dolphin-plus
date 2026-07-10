import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text_sensor
from esphome.const import CONF_ID

from . import DolphinBle

CONF_DOLPHIN_BLE_ID = "dolphin_ble_id"
CONF_KIND = "kind"

KINDS = {
    "robot_state": 0,
    "pws_state": 1,
    "cleaning_mode": 2,
    "in_water_status": 3,
    "pws_features": 4,
    "filter_status": 5,
    "wifi_ssid": 6,
    "quick_features": 7,
    "mu_sw_version": 8,
}

CONFIG_SCHEMA = text_sensor.text_sensor_schema().extend(
    {
        cv.Required(CONF_DOLPHIN_BLE_ID): cv.use_id(DolphinBle),
        cv.Required(CONF_KIND): cv.enum(KINDS, lower=True, space="_"),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_DOLPHIN_BLE_ID])
    var = await text_sensor.new_text_sensor(config)
    cg.add(parent.set_text_sensor(config[CONF_KIND], var))
    if config[CONF_KIND] in ("cleaning_mode", "in_water_status", "filter_status", "wifi_ssid", "mu_sw_version"):
        cg.add(var.publish_state("NA"))
