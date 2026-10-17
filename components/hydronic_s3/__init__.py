from esphome import automation
import esphome.codegen as cg
from esphome.components import canbus, time
import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome.const import CONF_ID, CONF_MODE, CONF_TIME_ID, CONF_TRIGGER_ID

CODEOWNERS = ["@farnham26"]
DEPENDENCIES = ["canbus"]
MULTI_CONF = False

CONF_HYDRONIC_S3_ID = "hydronic_s3_id"
CONF_RESEARCH = "research"
CONF_OBSERVER = "observer"
CONF_OTA_KEEPALIVE = "ota_keepalive"
CONF_ON_EVENT = "on_event"
CONF_ON_RUN_ENDED = "on_run_ended"
CONF_ON_NEW_FAULT_CODE = "on_new_fault_code"
CONF_LEASE = "lease"
CONF_POLL_INTERVAL = "poll_interval"
CONF_RUN = "run"  # not "on": YAML 1.1 reads that key as true

hydronic_s3_ns = cg.esphome_ns.namespace("hydronic_s3")
HydronicS3 = hydronic_s3_ns.class_("HydronicS3", cg.Component)
EventTrigger = hydronic_s3_ns.class_(
    "EventTrigger", automation.Trigger.template(cg.std_string, cg.int_, cg.int_)
)
RunEndedTrigger = hydronic_s3_ns.class_("RunEndedTrigger", automation.Trigger.template(cg.std_string))
NewFaultCodeTrigger = hydronic_s3_ns.class_(
    "NewFaultCodeTrigger", automation.Trigger.template(cg.std_string)
)
CommandAction = hydronic_s3_ns.class_("CommandAction", automation.Action)
CommandingCondition = hydronic_s3_ns.class_("CommandingCondition", automation.Condition)
BurnerOnCondition = hydronic_s3_ns.class_("BurnerOnCondition", automation.Condition)
BusyCondition = hydronic_s3_ns.class_("BusyCondition", automation.Condition)
StopUnsafeCondition = hydronic_s3_ns.class_("StopUnsafeCondition", automation.Condition)
ModeActiveCondition = hydronic_s3_ns.class_("ModeActiveCondition", automation.Condition)

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
        # OTA while the heater is busy: stop the run, keep talking during the upload and after
        # the reboot until it is idle (silence > ~15 s logs P000307 + P000440). ESP32 only.
        cv.Optional(CONF_OTA_KEEPALIVE, default=False): cv.All(cv.boolean, cv.only_on_esp32),
        # Clock sent to the heater (10A) with the config burst and on each sync.
        cv.Optional(CONF_TIME_ID): cv.use_id(time.RealTimeClock),
        # Sweep of the sensor DIDs (phase, temperatures, ...); stops by itself while the heater sleeps.
        cv.Optional(CONF_POLL_INTERVAL, default="5s"): cv.All(
            cv.positive_time_period_milliseconds, cv.Range(min=cv.TimePeriod(seconds=1))
        ),
        # Every transition (what, from, to), names unstable: research only.
        cv.Optional(CONF_ON_EVENT): automation.validate_automation(
            {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(EventTrigger)}
        ),
        # A run we commanded ended other than by our stop; never during a start (variable reason, log only).
        cv.Optional(CONF_ON_RUN_ENDED): automation.validate_automation(
            {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(RunEndedTrigger)}
        ),
        # A stored fault code not in the memory before (variable code, e.g. "P000250").
        cv.Optional(CONF_ON_NEW_FAULT_CODE): automation.validate_automation(
            {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(NewFaultCodeTrigger)}
        ),
    }
).extend(cv.COMPONENT_SCHEMA)


def _on_event_research_only(config):
    if CONF_ON_EVENT in config and not config[CONF_RESEARCH]:
        raise cv.Invalid(f"{CONF_ON_EVENT} needs {CONF_RESEARCH}: true (debug, names unstable)")
    return config


CONFIG_SCHEMA = cv.All(CONFIG_SCHEMA, _on_event_research_only)


def _final_validate(config):
    if config[CONF_OTA_KEEPALIVE] and "ota" not in fv.full_config.get():
        raise cv.Invalid(f"{CONF_OTA_KEEPALIVE} needs an ota: block")


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    bus = await cg.get_variable(config[canbus.CONF_CANBUS_ID])
    cg.add(var.set_canbus(bus))
    cg.add(var.set_research(config[CONF_RESEARCH]))
    cg.add(var.set_observer(config[CONF_OBSERVER]))
    if config[CONF_OTA_KEEPALIVE]:
        from esphome.components import ota

        ota.request_ota_state_listeners()
        cg.add_define("USE_HYDRONIC_S3_OTA_KEEPALIVE")
        cg.add(var.set_ota_keepalive(True))
    cg.add(var.set_poll_interval(config[CONF_POLL_INTERVAL]))
    if CONF_TIME_ID in config:
        clock = await cg.get_variable(config[CONF_TIME_ID])
        cg.add(var.set_time(clock))
    for conf in config.get(CONF_ON_EVENT, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(
            trigger, [(cg.std_string, "what"), (cg.int_, "from"), (cg.int_, "to")], conf
        )
    for conf in config.get(CONF_ON_RUN_ENDED, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [(cg.std_string, "reason")], conf)
    for conf in config.get(CONF_ON_NEW_FAULT_CODE, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [(cg.std_string, "code")], conf)


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


@automation.register_condition(
    "hydronic_s3.burner_on",
    BurnerOnCondition,
    automation.maybe_simple_id({cv.GenerateID(): cv.use_id(HydronicS3)}),
)
async def burner_on_to_code(config, condition_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(condition_id, template_arg, parent)


@automation.register_condition(
    "hydronic_s3.busy",
    BusyCondition,
    automation.maybe_simple_id({cv.GenerateID(): cv.use_id(HydronicS3)}),
)
async def busy_to_code(config, condition_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(condition_id, template_arg, parent)


@automation.register_condition(
    "hydronic_s3.mode_active",
    ModeActiveCondition,
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(HydronicS3),
            cv.Required(CONF_MODE): cv.templatable(cv.enum(COMMAND_MODES, upper=True)),
        }
    ),
)
async def mode_active_to_code(config, condition_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(condition_id, template_arg, parent)
    cg.add(var.set_mode(await cg.templatable(config[CONF_MODE], args, cg.uint8)))
    return var


@automation.register_condition(
    "hydronic_s3.stop_unsafe",
    StopUnsafeCondition,
    automation.maybe_simple_id({cv.GenerateID(): cv.use_id(HydronicS3)}),
)
async def stop_unsafe_to_code(config, condition_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(condition_id, template_arg, parent)
