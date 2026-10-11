import esphome.codegen as cg
from esphome.components import binary_sensor
import esphome.config_validation as cv
from esphome.const import DEVICE_CLASS_CONNECTIVITY, ENTITY_CATEGORY_DIAGNOSTIC

from . import CONF_HYDRONIC_S3_ID, HydronicS3

DEPENDENCIES = ["hydronic_s3"]

CONF_ON_REQUEST = "on_request"
CONF_FLAME = "flame"
CONF_CONNECTED = "connected"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_HYDRONIC_S3_ID): cv.use_id(HydronicS3),
        cv.Optional(CONF_ON_REQUEST): binary_sensor.binary_sensor_schema(),
        cv.Optional(CONF_FLAME): binary_sensor.binary_sensor_schema(),  # FD0F B0, polled
        # Heater status frames received within the stale timeout.
        cv.Optional(CONF_CONNECTED): binary_sensor.binary_sensor_schema(
            device_class=DEVICE_CLASS_CONNECTIVITY, entity_category=ENTITY_CATEGORY_DIAGNOSTIC
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_HYDRONIC_S3_ID])
    if conf := config.get(CONF_ON_REQUEST):
        cg.add(parent.set_on_request_binary_sensor(await binary_sensor.new_binary_sensor(conf)))
    if conf := config.get(CONF_FLAME):
        cg.add(parent.set_flame_binary_sensor(await binary_sensor.new_binary_sensor(conf)))
    if conf := config.get(CONF_CONNECTED):
        cg.add(parent.set_connected_binary_sensor(await binary_sensor.new_binary_sensor(conf)))
