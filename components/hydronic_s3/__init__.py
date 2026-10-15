from esphome import automation
import esphome.codegen as cg
from esphome.components import canbus, time
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_MODE, CONF_TIME_ID, CONF_TRIGGER_ID

CODEOWNERS = ["@farnham26"]
DEPENDENCIES = ["canbus"]
MULTI_CONF = False

CONF_HYDRONIC_S3_ID = "hydronic_s3_id"
CONF_RESEARCH = "research"
CONF_OBSERVER = "observer"
CONF_ON_EVENT = "on_event"
CONF_LEASE = "lease"
CONF_RUN = "run"  # not "on": YAML 1.1 reads that key as true

hydronic_s3_ns = cg.esphome_ns.namespace("hydronic_s3")
HydronicS3 = hydronic_s3_ns.class_("HydronicS3", cg.Component)
EventTrigger = hydronic_s3_ns.class_(
    "EventTrigger", automation.Trigger.template(cg.std_string, cg.int_, cg.int_)
)
CommandAction = hydronic_s3_ns.class_("CommandAction", automation.Action)
CommandingCondition = hydronic_s3_ns.class_("CommandingCondition", automation.Condition)

# 054 B1, as HydronicS3::MODE_*.
COMMAND_MODES = {"HEAT": 0x01, "FAN": 0x02, "RESIDUAL": 0x03}

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(HydronicS3),
        cv.GenerateID(canbus.CONF_CANBUS_ID): cv.use_id(canbus.CanbusComponent),
        # Per-ID table, change log and wake trace in the log (protocol research).
        cv.Optional(CONF_RESEARCH, default=False): cv.boolean,
        # Diagnosis only, never a control unit (another unit controls the heater).
        cv.Optional(CONF_OBSERVER, default=False): cv.boolean,
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
    cg.add(var.set_observer(config[CONF_OBSERVER]))
    if CONF_TIME_ID in config:
        clock = await cg.get_variable(config[CONF_TIME_ID])
        cg.add(var.set_time(clock))
    for conf in config.get(CONF_ON_EVENT, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(
            trigger, [(cg.std_string, "what"), (cg.int_, "from"), (cg.int_, "to")], conf
        )


@automation.register_action(
    "hydronic_s3.command",
    CommandAction,
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(HydronicS3),
            cv.Required(CONF_RUN): cv.templatable(cv.boolean),
            cv.Optional(CONF_MODE): cv.templatable(cv.enum(COMMAND_MODES, upper=True)),
            # Watchdog (set_command_lease), 0 = none.
            cv.Optional(CONF_LEASE): cv.templatable(cv.positive_time_period_milliseconds),
        }
    ),
    synchronous=True,
)
async def command_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, parent)
    cg.add(var.set_run(await cg.templatable(config[CONF_RUN], args, bool)))
    if CONF_MODE in config:
        cg.add(var.set_mode(await cg.templatable(config[CONF_MODE], args, cg.uint8)))
    if CONF_LEASE in config:
        cg.add(var.set_lease(await cg.templatable(config[CONF_LEASE], args, cg.uint32)))
    return var


@automation.register_condition(
    "hydronic_s3.commanding",
    CommandingCondition,
    automation.maybe_simple_id({cv.GenerateID(): cv.use_id(HydronicS3)}),
)
async def commanding_to_code(config, condition_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(condition_id, template_arg, parent)
