import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import select
from esphome.const import CONF_ID

from . import DolphinBle

CONF_DOLPHIN_BLE_ID = "dolphin_ble_id"
CONF_KIND = "kind"

KINDS = {
    "cleaning_mode": 0,
    "manual_drive_direction": 1,
    "monday_mode": 2,
    "tuesday_mode": 3,
    "wednesday_mode": 4,
    "thursday_mode": 5,
    "friday_mode": 6,
    "saturday_mode": 7,
    "sunday_mode": 8,
}

OPTIONS = {
    0: [
        "All Surfaces",
        "Quick Clean",
        "Cove",
        "Floor Only",
        "Water Line",
        "Ultra Clean",
        "Spot",
        "Wall Only",
        "Tic Tac",
        "Custom",
        "Pickup",
    ],
    1: ["Stop", "Forward", "Backward", "Right", "Left"],
}

# Day mode options prepend "Disabled" to the standard cleaning modes
for k in range(2, 9):
    OPTIONS[k] = ["Disabled"] + OPTIONS[0]

dolphin_ble_ns = cg.esphome_ns.namespace("dolphin_ble")
DolphinBleSelect = dolphin_ble_ns.class_("DolphinBleSelect", select.Select)

CONFIG_SCHEMA = select.select_schema(
    DolphinBleSelect,
    icon="mdi:robot-vacuum",
).extend(
    {
        cv.Required(CONF_DOLPHIN_BLE_ID): cv.use_id(DolphinBle),
        cv.Required(CONF_KIND): cv.enum(KINDS, lower=True, space="_"),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_DOLPHIN_BLE_ID])
    kind = KINDS[config[CONF_KIND]]
    var = await select.new_select(config, parent, kind, options=OPTIONS[kind])
    if kind == 0:
        cg.add(parent.set_cleaning_mode_select(var))
    elif kind == 1:
        cg.add(parent.set_manual_drive_direction_select(var))
    else:
        cg.add(parent.set_day_mode_select(kind - 2, var))
