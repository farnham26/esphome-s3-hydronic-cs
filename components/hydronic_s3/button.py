import esphome.codegen as cg
from esphome.components import button
import esphome.config_validation as cv

from . import CONF_HYDRONIC_S3_ID, HydronicS3, hydronic_s3_ns

DEPENDENCIES = ["hydronic_s3"]

CONF_READ_FAULT_CODES = "read_fault_codes"
CONF_CLEAR_FAULT_CODES = "clear_fault_codes"

ReadFaultCodesButton = hydronic_s3_ns.class_(
    "ReadFaultCodesButton", button.Button, cg.Parented.template(HydronicS3)
)
ClearFaultCodesButton = hydronic_s3_ns.class_(
    "ClearFaultCodesButton", button.Button, cg.Parented.template(HydronicS3)
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_HYDRONIC_S3_ID): cv.use_id(HydronicS3),
        # Error memory as the EasyStart's workshop menu reads it.
        cv.Optional(CONF_READ_FAULT_CODES): button.button_schema(
            ReadFaultCodesButton, icon="mdi:clipboard-search-outline"
        ),
        # Needs a SecurityAccess provider (set_security_provider()), else fails with a message.
        cv.Optional(CONF_CLEAR_FAULT_CODES): button.button_schema(
            ClearFaultCodesButton, icon="mdi:delete-sweep-outline"
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_HYDRONIC_S3_ID])
    for key in (CONF_READ_FAULT_CODES, CONF_CLEAR_FAULT_CODES):
        if conf := config.get(key):
            btn = await button.new_button(conf)
            await cg.register_parented(btn, parent)
