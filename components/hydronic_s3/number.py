import esphome.codegen as cg
from esphome.components import number
import esphome.config_validation as cv
from esphome.const import UNIT_MINUTE

from . import CONF_HYDRONIC_S3_ID, HydronicS3, hydronic_s3_ns

DEPENDENCIES = ["hydronic_s3"]

CONF_RUN_WATCHDOG = "run_watchdog"

RunWatchdogNumber = hydronic_s3_ns.class_(
    "RunWatchdogNumber", number.Number, cg.Component, cg.Parented.template(HydronicS3)
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_HYDRONIC_S3_ID): cv.use_id(HydronicS3),
        # 0 = run until turned off; > 0: the run stops unless turned on again within this time.
        cv.Optional(CONF_RUN_WATCHDOG): number.number_schema(
            RunWatchdogNumber, icon="mdi:timer-cog-outline", unit_of_measurement=UNIT_MINUTE
        ).extend(cv.COMPONENT_SCHEMA),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_HYDRONIC_S3_ID])
    if conf := config.get(CONF_RUN_WATCHDOG):
        num = await number.new_number(conf, min_value=0, max_value=600, step=1)
        await cg.register_component(num, conf)
        await cg.register_parented(num, parent)
