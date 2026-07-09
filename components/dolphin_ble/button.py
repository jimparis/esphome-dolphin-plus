import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import button
from esphome.const import CONF_ID

from . import DolphinBle

CONF_DOLPHIN_BLE_ID = "dolphin_ble_id"
CONF_KIND = "kind"

KINDS = {
    "start_cleaning": 0,
    "stop_cleaning": 1,
    "pickup_mode": 2,
    "refresh_status": 3,
    "manual_drive": 4,
    "quit_manual_drive": 5,
}

dolphin_ble_ns = cg.esphome_ns.namespace("dolphin_ble")
DolphinBleButton = dolphin_ble_ns.class_("DolphinBleButton", button.Button)

CONFIG_SCHEMA = button.button_schema(DolphinBleButton, icon="mdi:robot-vacuum").extend(
    {
        cv.Required(CONF_DOLPHIN_BLE_ID): cv.use_id(DolphinBle),
        cv.Required(CONF_KIND): cv.enum(KINDS, lower=True, space="_"),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_DOLPHIN_BLE_ID])
    var = await button.new_button(config, parent, config[CONF_KIND])
    # The button entity is self-contained; no additional registration is needed.
