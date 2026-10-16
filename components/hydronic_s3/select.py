import esphome.codegen as cg
from esphome.components import select
import esphome.config_validation as cv

from . import CONF_HYDRONIC_S3_ID, HydronicS3, hydronic_s3_ns

DEPENDENCIES = ["hydronic_s3"]

CONF_MODE = "mode"

ModeSelect = hydronic_s3_ns.class_("ModeSelect", select.Select, cg.Component, cg.Parented.template(HydronicS3))

# Order as ModeSelect's index (hydronic_s3.cpp); "S+" is display only.
MODE_OPTIONS = ["Heat", "Residual heat", "Fan", "S+"]

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_HYDRONIC_S3_ID): cv.use_id(HydronicS3),
        # What "Heater Run" starts; restored.
        cv.Optional(CONF_MODE): select.select_schema(ModeSelect, icon="mdi:tune-variant").extend(
            cv.COMPONENT_SCHEMA
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_HYDRONIC_S3_ID])
    if conf := config.get(CONF_MODE):
        sel = await select.new_select(conf, options=MODE_OPTIONS)
        await cg.register_component(sel, conf)
        await cg.register_parented(sel, parent)
        cg.add(parent.set_mode_select(sel))
