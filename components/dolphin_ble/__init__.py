import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import esp32_ble
from esphome.const import CONF_ID, CONF_MAC_ADDRESS, CONF_NAME

CODEOWNERS = ["@local"]
DEPENDENCIES = ["esp32_ble"]

CONF_NAME_FILTER = "name_filter"
CONF_AUTO_PROBE = "auto_probe"
CONF_REPEAT_PROBES = "repeat_probes"
CONF_PROBES = "probes"
CONF_PACKET = "packet"
CONF_TEXT = "text"
CONF_DELAY_MS = "delay_ms"

dolphin_ble_ns = cg.esphome_ns.namespace("dolphin_ble")
DolphinBle = dolphin_ble_ns.class_("DolphinBle", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(DolphinBle),
        cv.Required(CONF_MAC_ADDRESS): cv.mac_address,
        cv.Optional(CONF_NAME_FILTER, default=""): cv.string,
        cv.Optional(CONF_AUTO_PROBE, default=False): cv.boolean,
        cv.Optional(CONF_REPEAT_PROBES, default=False): cv.boolean,
        cv.Optional(CONF_PROBES, default=[]): cv.ensure_list(
            {
                cv.Required(CONF_NAME): cv.string,
                cv.Exclusive(CONF_PACKET, "probe_payload"): cv.string,
                cv.Exclusive(CONF_TEXT, "probe_payload"): cv.string,
                cv.Optional(CONF_DELAY_MS, default=1500): cv.positive_int,
            }
        ),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_mac_address(str(config[CONF_MAC_ADDRESS])))
    cg.add(var.set_name_filter(config[CONF_NAME_FILTER]))
    cg.add(var.set_auto_probe(config[CONF_AUTO_PROBE]))
    cg.add(var.set_repeat_probes(config[CONF_REPEAT_PROBES]))
    for probe in config[CONF_PROBES]:
        if CONF_TEXT in probe:
            cg.add(var.add_text_probe(probe[CONF_NAME], probe[CONF_TEXT], probe[CONF_DELAY_MS]))
        else:
            cg.add(var.add_probe(probe[CONF_NAME], probe[CONF_PACKET], probe[CONF_DELAY_MS]))
