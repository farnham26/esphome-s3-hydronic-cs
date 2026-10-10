import esphome.codegen as cg
from esphome.components import binary_sensor
import esphome.config_validation as cv

from . import CONF_HYDRONIC_S3_ID, HydronicS3

DEPENDENCIES = ["hydronic_s3"]

CONF_ON_REQUEST = "on_request"
CONF_FLAME = "flame"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_HYDRONIC_S3_ID): cv.use_id(HydronicS3),
        cv.Optional(CONF_ON_REQUEST): binary_sensor.binary_sensor_schema(),
        cv.Optional(CONF_FLAME): binary_sensor.binary_sensor_schema(),  # FD0F B0, polled
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_HYDRONIC_S3_ID])
    if conf := config.get(CONF_ON_REQUEST):
        cg.add(parent.set_on_request_binary_sensor(await binary_sensor.new_binary_sensor(conf)))
    if conf := config.get(CONF_FLAME):
        cg.add(parent.set_flame_binary_sensor(await binary_sensor.new_binary_sensor(conf)))
