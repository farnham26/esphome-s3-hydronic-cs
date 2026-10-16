import esphome.codegen as cg
from esphome.components import switch
import esphome.config_validation as cv
from esphome.const import CONF_TYPE, ENTITY_CATEGORY_CONFIG

from . import CONF_HYDRONIC_S3_ID, HydronicS3, hydronic_s3_ns

DEPENDENCIES = ["hydronic_s3"]

CONF_RUN = "run"
CONF_POLLING = "polling"

RunSwitch = hydronic_s3_ns.class_("RunSwitch", switch.Switch, cg.Component, cg.Parented.template(HydronicS3))
PollingSwitch = hydronic_s3_ns.class_(
    "PollingSwitch", switch.Switch, cg.Component, cg.Parented.template(HydronicS3)
)

# One entity per item (type: run | polling): an id at the top level, so packages can !extend it.
CONFIG_SCHEMA = cv.typed_schema(
    {
        # Start/stop (set_command); reads back refusals and stops by the component.
        CONF_RUN: switch.switch_schema(RunSwitch, icon="mdi:fire", default_restore_mode="ALWAYS_OFF")
        .extend({cv.GenerateID(CONF_HYDRONIC_S3_ID): cv.use_id(HydronicS3)})
        .extend(cv.COMPONENT_SCHEMA),
        # Polling of the sensor DIDs (default: on at boot).
        CONF_POLLING: switch.switch_schema(
            PollingSwitch, entity_category=ENTITY_CATEGORY_CONFIG, default_restore_mode="ALWAYS_ON"
        )
        .extend({cv.GenerateID(CONF_HYDRONIC_S3_ID): cv.use_id(HydronicS3)})
        .extend(cv.COMPONENT_SCHEMA),
    },
    key=CONF_TYPE,
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_HYDRONIC_S3_ID])
    sw = await switch.new_switch(config)
    await cg.register_component(sw, config)
    await cg.register_parented(sw, parent)
    if config[CONF_TYPE] == CONF_RUN:
        cg.add(parent.set_run_switch(sw))
    else:
        cg.add(parent.set_polling_switch(sw))
