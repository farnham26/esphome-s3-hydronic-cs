from esphome import automation
import esphome.codegen as cg
from esphome.components import canbus, time
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_TIME_ID, CONF_TRIGGER_ID

CODEOWNERS = ["@farnham26"]
DEPENDENCIES = ["canbus"]
MULTI_CONF = False

CONF_HYDRONIC_S3_ID = "hydronic_s3_id"
CONF_RESEARCH = "research"
CONF_ON_EVENT = "on_event"

hydronic_s3_ns = cg.esphome_ns.namespace("hydronic_s3")
HydronicS3 = hydronic_s3_ns.class_("HydronicS3", cg.Component)
EventTrigger = hydronic_s3_ns.class_(
    "EventTrigger", automation.Trigger.template(cg.std_string, cg.int_, cg.int_)
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(HydronicS3),
        cv.GenerateID(canbus.CONF_CANBUS_ID): cv.use_id(canbus.CanbusComponent),
        # Per-ID table, change log and wake trace in the log (protocol research).
        cv.Optional(CONF_RESEARCH, default=False): cv.boolean,
        # Clock sent to the heater (10A) with the config burst and on each sync.
        cv.Optional(CONF_TIME_ID): cv.use_id(time.RealTimeClock),
        # Heater transitions: what (e.g. "control_pause", "state"), from, to.
        cv.Optional(CONF_ON_EVENT): automation.validate_automation(
            {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(EventTrigger)}
        ),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    bus = await cg.get_variable(config[canbus.CONF_CANBUS_ID])
    cg.add(var.set_canbus(bus))
    cg.add(var.set_research(config[CONF_RESEARCH]))
    if CONF_TIME_ID in config:
        clock = await cg.get_variable(config[CONF_TIME_ID])
        cg.add(var.set_time(clock))
    for conf in config.get(CONF_ON_EVENT, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(
            trigger, [(cg.std_string, "what"), (cg.int_, "from"), (cg.int_, "to")], conf
        )
