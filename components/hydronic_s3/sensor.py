import esphome.codegen as cg
from esphome.components import sensor
import esphome.config_validation as cv
from esphome.const import (
    DEVICE_CLASS_TEMPERATURE,
    STATE_CLASS_MEASUREMENT,
    UNIT_CELSIUS,
)

from . import CONF_HYDRONIC_S3_ID, HydronicS3

DEPENDENCIES = ["hydronic_s3"]

CONF_OUTLET_TEMPERATURE = "outlet_temperature"
CONF_STATE_CODE = "state_code"
CONF_CONTROLLER_TEMPERATURE = "controller_temperature"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_HYDRONIC_S3_ID): cv.use_id(HydronicS3),
        cv.Optional(CONF_OUTLET_TEMPERATURE): sensor.sensor_schema(
            unit_of_measurement=UNIT_CELSIUS,
            accuracy_decimals=1,
            device_class=DEVICE_CLASS_TEMPERATURE,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_STATE_CODE): sensor.sensor_schema(accuracy_decimals=0),
        cv.Optional(CONF_CONTROLLER_TEMPERATURE): sensor.sensor_schema(
            unit_of_measurement=UNIT_CELSIUS,
            accuracy_decimals=1,
            device_class=DEVICE_CLASS_TEMPERATURE,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_HYDRONIC_S3_ID])
    if conf := config.get(CONF_OUTLET_TEMPERATURE):
        cg.add(parent.set_outlet_temperature_sensor(await sensor.new_sensor(conf)))
    if conf := config.get(CONF_STATE_CODE):
        cg.add(parent.set_state_code_sensor(await sensor.new_sensor(conf)))
    if conf := config.get(CONF_CONTROLLER_TEMPERATURE):
        cg.add(parent.set_controller_temperature_sensor(await sensor.new_sensor(conf)))
