import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text
from esphome.const import CONF_ID

from . import DolphinBle

CONF_DOLPHIN_BLE_ID = "dolphin_ble_id"
CONF_KIND = "kind"

KINDS = {
    "monday_time": 0,
    "tuesday_time": 1,
    "wednesday_time": 2,
    "thursday_time": 3,
    "friday_time": 4,
    "saturday_time": 5,
    "sunday_time": 6,
}

dolphin_ble_ns = cg.esphome_ns.namespace("dolphin_ble")
DolphinBleText = dolphin_ble_ns.class_("DolphinBleText", text.Text)

CONFIG_SCHEMA = text.text_schema(DolphinBleText, icon="mdi:clock").extend(
    {
        cv.Required(CONF_DOLPHIN_BLE_ID): cv.use_id(DolphinBle),
        cv.Required(CONF_KIND): cv.enum(KINDS, lower=True, space="_"),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_DOLPHIN_BLE_ID])
    kind = KINDS[config[CONF_KIND]]
    var = cg.new_Pvariable(config[CONF_ID], parent, kind)
    await text.register_text(var, config)
    cg.add(parent.set_day_time_text(kind, var))
