import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import light
from esphome.const import CONF_OUTPUT_ID, CONF_ID

from . import DolphinBle, dolphin_ble_ns

CONF_DOLPHIN_BLE_ID = "dolphin_ble_id"

DolphinBleLight = dolphin_ble_ns.class_("DolphinBleLight", light.LightOutput, cg.Component)

CONFIG_SCHEMA = light.BRIGHTNESS_ONLY_LIGHT_SCHEMA.extend(
    {
        cv.GenerateID(CONF_OUTPUT_ID): cv.declare_id(DolphinBleLight),
        cv.Required(CONF_DOLPHIN_BLE_ID): cv.use_id(DolphinBle),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_DOLPHIN_BLE_ID])
    var = cg.new_Pvariable(config[CONF_OUTPUT_ID], parent)
    await cg.register_component(var, config)
    await light.register_light(var, config)
    state = await cg.get_variable(config[CONF_ID])
    cg.add(parent.set_led_light(state))
