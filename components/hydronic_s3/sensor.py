import esphome.codegen as cg
from esphome.components import sensor
import esphome.config_validation as cv
from esphome.const import (
    DEVICE_CLASS_CURRENT,
    DEVICE_CLASS_DURATION,
    DEVICE_CLASS_POWER,
    DEVICE_CLASS_TEMPERATURE,
    DEVICE_CLASS_VOLTAGE,
    DEVICE_CLASS_VOLUME,
    DEVICE_CLASS_VOLUME_FLOW_RATE,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_AMPERE,
    UNIT_CELSIUS,
    UNIT_HERTZ,
    UNIT_HOUR,
    UNIT_REVOLUTIONS_PER_MINUTE,
    UNIT_VOLT,
    UNIT_WATT,
)

from . import CONF_HYDRONIC_S3_ID, HydronicS3

DEPENDENCIES = ["hydronic_s3"]

CONF_OUTLET_TEMPERATURE = "outlet_temperature"
CONF_STATE_CODE = "state_code"

DidSensor = cg.esphome_ns.namespace("hydronic_s3").enum("DidSensor")

_TEMP = dict(
    unit_of_measurement=UNIT_CELSIUS,
    accuracy_decimals=1,
    device_class=DEVICE_CLASS_TEMPERATURE,
    state_class=STATE_CLASS_MEASUREMENT,
)
_HOURS = dict(
    unit_of_measurement=UNIT_HOUR,
    accuracy_decimals=1,
    device_class=DEVICE_CLASS_DURATION,
    state_class=STATE_CLASS_TOTAL_INCREASING,
    entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
)
# Polled by UDS 22 (start_polling with HydronicS3::sensor_dids()).
DID_SENSORS = {
    "inlet_temperature": ("DID_INLET_TEMPERATURE", _TEMP),
    "flame_sensor_temperature": ("DID_FLAME_SENSOR_TEMP", _TEMP),
    "supply_voltage": (
        "DID_SUPPLY_VOLTAGE",
        dict(
            unit_of_measurement=UNIT_VOLT,
            accuracy_decimals=1,
            device_class=DEVICE_CLASS_VOLTAGE,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
    ),
    "blower_speed": (
        "DID_BLOWER_SPEED",
        dict(
            unit_of_measurement=UNIT_REVOLUTIONS_PER_MINUTE,
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
    ),
    "glow_plug_power": (
        "DID_GLOW_PLUG_POWER",
        dict(
            unit_of_measurement=UNIT_WATT,
            accuracy_decimals=1,
            device_class=DEVICE_CLASS_POWER,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
    ),
    "glow_plug_raw": ("DID_GLOW_PLUG_RAW", dict(accuracy_decimals=0)),
    **{
        key: (
            did,
            dict(
                unit_of_measurement=UNIT_AMPERE,
                accuracy_decimals=2,
                device_class=DEVICE_CLASS_CURRENT,
                state_class=STATE_CLASS_MEASUREMENT,
            ),
        )
        for key, did in (
            ("water_pump_current", "DID_WATER_PUMP_CURRENT"),
            ("fuel_pump_current", "DID_FUEL_PUMP_CURRENT"),
        )
    },
    "water_pump_power": (
        "DID_WATER_PUMP_POWER",
        dict(
            unit_of_measurement=UNIT_WATT,
            accuracy_decimals=1,
            device_class=DEVICE_CLASS_POWER,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
    ),
    "metering_pump_rate": (
        "DID_METERING_PUMP_RATE",
        dict(
            unit_of_measurement=UNIT_HERTZ,
            accuracy_decimals=1,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
    ),
    "operating_phase": ("DID_OPERATING_PHASE", dict(accuracy_decimals=0)),
    "fuel_consumption": (
        "DID_FUEL_CONSUMPTION",
        dict(
            unit_of_measurement="L/h",
            accuracy_decimals=3,
            device_class=DEVICE_CLASS_VOLUME_FLOW_RATE,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
    ),
    "heating_power": (
        "DID_HEATING_POWER",
        dict(
            unit_of_measurement=UNIT_WATT,
            accuracy_decimals=0,
            device_class=DEVICE_CLASS_POWER,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
    ),
    "fuel_total": (
        "DID_FUEL_TOTAL",
        dict(
            unit_of_measurement="L",
            accuracy_decimals=3,
            device_class=DEVICE_CLASS_VOLUME,
            state_class=STATE_CLASS_TOTAL_INCREASING,
        ),
    ),
    "hours_total": ("DID_HOURS_TOTAL", _HOURS),
    "hours_power_0_25": ("DID_HOURS_POWER_0_25", _HOURS),
    "hours_power_25_50": ("DID_HOURS_POWER_25_50", _HOURS),
    "hours_power_50_75": ("DID_HOURS_POWER_50_75", _HOURS),
    "hours_power_75_100": ("DID_HOURS_POWER_75_100", _HOURS),
    "hours_pause": ("DID_HOURS_PAUSE", _HOURS),
    "hours_altitude": ("DID_HOURS_ALTITUDE", _HOURS),
    "fd08_raw": ("DID_FD08_RAW", dict(accuracy_decimals=0, entity_category=ENTITY_CATEGORY_DIAGNOSTIC)),
    "start_step": ("DID_START_STEP", dict(accuracy_decimals=0)),
    "combustion_state": ("DID_COMBUSTION_STATE", dict(accuracy_decimals=0)),
    "control_unit_temperature": ("DID_CONTROL_UNIT_TEMP", _TEMP),
    "water_flow": (
        "DID_WATER_FLOW",
        dict(
            unit_of_measurement="L/h",
            accuracy_decimals=0,
            device_class=DEVICE_CLASS_VOLUME_FLOW_RATE,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
    ),
    "fd35_raw": ("DID_FD35_RAW", dict(accuracy_decimals=0, entity_category=ENTITY_CATEGORY_DIAGNOSTIC)),
}

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
        **{cv.Optional(key): sensor.sensor_schema(**kw) for key, (_, kw) in DID_SENSORS.items()},
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_HYDRONIC_S3_ID])
    if conf := config.get(CONF_OUTLET_TEMPERATURE):
        cg.add(parent.set_outlet_temperature_sensor(await sensor.new_sensor(conf)))
    if conf := config.get(CONF_STATE_CODE):
        cg.add(parent.set_state_code_sensor(await sensor.new_sensor(conf)))
    for key, (which, _) in DID_SENSORS.items():
        if conf := config.get(key):
            cg.add(parent.set_did_sensor(getattr(DidSensor, which), await sensor.new_sensor(conf)))
