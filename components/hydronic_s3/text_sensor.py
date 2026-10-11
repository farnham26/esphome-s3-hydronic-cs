import esphome.codegen as cg
from esphome.components import text_sensor
import esphome.config_validation as cv

from . import CONF_HYDRONIC_S3_ID, HydronicS3

DEPENDENCIES = ["hydronic_s3"]

CONF_STATE = "state"
CONF_FAULT = "fault"
CONF_EVENT = "event"
CONF_FAULT_CODES = "fault_codes"
CONF_DIAG_RESPONSE = "diag_response"
CONF_OPERATING_PHASE_TEXT = "operating_phase_text"
CONF_COMBUSTION_TEXT = "combustion_text"
CONF_REQUEST_SOURCE = "request_source"
IdentText = cg.esphome_ns.namespace("hydronic_s3").enum("IdentText")
IDENT = {
    "hardware_version": "IDENT_HARDWARE_VERSION",
    "software_version": "IDENT_SOFTWARE_VERSION",
    "part_number": "IDENT_PART_NUMBER",
    "serial_number": "IDENT_SERIAL",
}

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_HYDRONIC_S3_ID): cv.use_id(HydronicS3),
        cv.Optional(CONF_STATE): text_sensor.text_sensor_schema(),
        cv.Optional(CONF_FAULT): text_sensor.text_sensor_schema(),
        cv.Optional(CONF_EVENT): text_sensor.text_sensor_schema(),
        cv.Optional(CONF_FAULT_CODES): text_sensor.text_sensor_schema(),
        cv.Optional(CONF_DIAG_RESPONSE): text_sensor.text_sensor_schema(),
        cv.Optional(CONF_OPERATING_PHASE_TEXT): text_sensor.text_sensor_schema(),  # FD10 B1, descriptive
        cv.Optional(CONF_COMBUSTION_TEXT): text_sensor.text_sensor_schema(),  # FD0F B1, named
        cv.Optional(CONF_REQUEST_SOURCE): text_sensor.text_sensor_schema(),  # FD10 B0 source, inferred
        **{
            cv.Optional(key): text_sensor.text_sensor_schema(entity_category="diagnostic")
            for key in IDENT
        },
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_HYDRONIC_S3_ID])
    if conf := config.get(CONF_STATE):
        cg.add(parent.set_state_text_sensor(await text_sensor.new_text_sensor(conf)))
    if conf := config.get(CONF_FAULT):
        cg.add(parent.set_fault_text_sensor(await text_sensor.new_text_sensor(conf)))
    if conf := config.get(CONF_EVENT):
        cg.add(parent.set_event_text_sensor(await text_sensor.new_text_sensor(conf)))
    if conf := config.get(CONF_FAULT_CODES):
        cg.add(parent.set_fault_codes_text_sensor(await text_sensor.new_text_sensor(conf)))
    if conf := config.get(CONF_DIAG_RESPONSE):
        cg.add(parent.set_diag_response_text_sensor(await text_sensor.new_text_sensor(conf)))
    if conf := config.get(CONF_OPERATING_PHASE_TEXT):
        cg.add(parent.set_phase_text_sensor(await text_sensor.new_text_sensor(conf)))
    if conf := config.get(CONF_COMBUSTION_TEXT):
        cg.add(parent.set_combustion_text_sensor(await text_sensor.new_text_sensor(conf)))
    if conf := config.get(CONF_REQUEST_SOURCE):
        cg.add(parent.set_request_source_text_sensor(await text_sensor.new_text_sensor(conf)))
    for key, which in IDENT.items():
        if conf := config.get(key):
            cg.add(parent.set_ident_text_sensor(getattr(IdentText, which), await text_sensor.new_text_sensor(conf)))
