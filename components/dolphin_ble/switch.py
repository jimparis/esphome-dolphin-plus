import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import switch
from esphome.const import CONF_ID

from . import DolphinBle

CONF_DOLPHIN_BLE_ID = "dolphin_ble_id"
CONF_KIND = "kind"

KINDS = {
    "weekly_repeat": 0,
    "protocol_debug_logging": 1,
}

dolphin_ble_ns = cg.esphome_ns.namespace("dolphin_ble")
DolphinBleSwitch = dolphin_ble_ns.class_("DolphinBleSwitch", switch.Switch)

CONFIG_SCHEMA = switch.switch_schema(DolphinBleSwitch, icon="mdi:repeat").extend(
    {
        cv.Required(CONF_DOLPHIN_BLE_ID): cv.use_id(DolphinBle),
        cv.Required(CONF_KIND): cv.enum(KINDS, lower=True, space="_"),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_DOLPHIN_BLE_ID])
    kind = KINDS[config[CONF_KIND]]
    var = await switch.new_switch(config, parent, kind)
    if kind == 0:
        cg.add(parent.set_weekly_repeat_switch(var))
    else:
        cg.add(parent.set_protocol_debug_switch(var))
