import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import number
from esphome.const import CONF_ID

from . import DolphinBle

CONF_DOLPHIN_BLE_ID = "dolphin_ble_id"

dolphin_ble_ns = cg.esphome_ns.namespace("dolphin_ble")
DolphinBleNumber = dolphin_ble_ns.class_("DolphinBleNumber", number.Number)

CONFIG_SCHEMA = number.number_schema(
    DolphinBleNumber,
    icon="mdi:speedometer",
).extend(
    {
        cv.Required(CONF_DOLPHIN_BLE_ID): cv.use_id(DolphinBle),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_DOLPHIN_BLE_ID])
    var = await number.new_number(
        config,
        parent,
        min_value=0.0,
        max_value=100.0,
        step=1.0,
    )
    cg.add(var.publish_state(50.0))
    cg.add(parent.set_manual_drive_speed(50.0))
