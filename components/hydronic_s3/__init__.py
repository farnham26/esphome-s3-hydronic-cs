import esphome.codegen as cg
from esphome.components import canbus
import esphome.config_validation as cv
from esphome.const import CONF_ID

CODEOWNERS = ["@farnham26"]
DEPENDENCIES = ["canbus"]
MULTI_CONF = False

CONF_HYDRONIC_S3_ID = "hydronic_s3_id"
CONF_RESEARCH = "research"

hydronic_s3_ns = cg.esphome_ns.namespace("hydronic_s3")
HydronicS3 = hydronic_s3_ns.class_("HydronicS3", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(HydronicS3),
        cv.GenerateID(canbus.CONF_CANBUS_ID): cv.use_id(canbus.CanbusComponent),
        # Per-ID table, change log and wake trace in the log (protocol research).
        cv.Optional(CONF_RESEARCH, default=False): cv.boolean,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    bus = await cg.get_variable(config[canbus.CONF_CANBUS_ID])
    cg.add(var.set_canbus(bus))
    cg.add(var.set_research(config[CONF_RESEARCH]))
