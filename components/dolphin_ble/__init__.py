import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import esp32_ble
from esphome.const import CONF_ID, CONF_MAC_ADDRESS

CODEOWNERS = ["@local"]
DEPENDENCIES = ["esp32_ble"]

CONF_NAME_FILTER = "name_filter"

dolphin_ble_ns = cg.esphome_ns.namespace("dolphin_ble")
DolphinBle = dolphin_ble_ns.class_("DolphinBle", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(DolphinBle),
        cv.Required(CONF_MAC_ADDRESS): cv.mac_address,
        cv.Optional(CONF_NAME_FILTER, default=""): cv.string,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_mac_address(str(config[CONF_MAC_ADDRESS])))
    cg.add(var.set_name_filter(config[CONF_NAME_FILTER]))
