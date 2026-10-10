import esphome.codegen as cg
from esphome.components import text_sensor
import esphome.config_validation as cv

from . import CONF_HYDRONIC_S3_ID, HydronicS3

DEPENDENCIES = ["hydronic_s3"]

CONF_STATE = "state"
CONF_FAULT = "fault"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_HYDRONIC_S3_ID): cv.use_id(HydronicS3),
        cv.Optional(CONF_STATE): text_sensor.text_sensor_schema(),
        cv.Optional(CONF_FAULT): text_sensor.text_sensor_schema(),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_HYDRONIC_S3_ID])
    if conf := config.get(CONF_STATE):
        cg.add(parent.set_state_text_sensor(await text_sensor.new_text_sensor(conf)))
    if conf := config.get(CONF_FAULT):
        cg.add(parent.set_fault_text_sensor(await text_sensor.new_text_sensor(conf)))
