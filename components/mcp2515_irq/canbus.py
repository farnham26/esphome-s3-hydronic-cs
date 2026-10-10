# Interrupt-driven fork of ESPHome core mcp2515 (2026.9.1).
from esphome import pins
import esphome.codegen as cg
from esphome.components import canbus, esp32, spi
from esphome.components.canbus import CanbusComponent
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_INTERRUPT_PIN, CONF_MODE, CONF_RX_QUEUE_LEN
from esphome.types import ConfigType

CODEOWNERS = ["@farnham26"]
DEPENDENCIES = ["spi"]

CONF_CLOCK = "clock"

mcp2515_ns = cg.esphome_ns.namespace("mcp2515_irq")
mcp2515 = mcp2515_ns.class_("MCP2515", CanbusComponent, spi.SPIDevice)
CanClock = mcp2515_ns.enum("CAN_CLOCK")
McpMode = mcp2515_ns.enum("CANCTRL_REQOP_MODE")

CAN_CLOCK = {
    "8MHZ": CanClock.MCP_8MHZ,
    "12MHZ": CanClock.MCP_12MHZ,
    "16MHZ": CanClock.MCP_16MHZ,
    "20MHZ": CanClock.MCP_20MHZ,
}

MCP_MODE = {
    "NORMAL": McpMode.CANCTRL_REQOP_NORMAL,
    "LOOPBACK": McpMode.CANCTRL_REQOP_LOOPBACK,
    "LISTENONLY": McpMode.CANCTRL_REQOP_LISTENONLY,
}

CONFIG_SCHEMA = canbus.CANBUS_SCHEMA.extend(
    {
        cv.GenerateID(): cv.declare_id(mcp2515),
        cv.Optional(CONF_CLOCK, default="8MHZ"): cv.enum(CAN_CLOCK, upper=True),
        cv.Optional(CONF_MODE, default="NORMAL"): cv.enum(MCP_MODE, upper=True),
        # Without it, frames are polled from the main loop as in core mcp2515.
        cv.Optional(CONF_INTERRUPT_PIN): pins.internal_gpio_input_pin_schema,
        cv.Optional(CONF_RX_QUEUE_LEN, default=256): cv.int_range(min=8, max=4096),
    }
).extend(spi.spi_device_schema(True))


async def to_code(config: ConfigType) -> None:
    rhs = mcp2515.new()
    var = cg.Pvariable(config[CONF_ID], rhs)
    await canbus.register_canbus(var, config)
    if CONF_CLOCK in config:
        canclock = CAN_CLOCK[config[CONF_CLOCK]]
        cg.add(var.set_mcp_clock(canclock))
    if CONF_MODE in config:
        mode = MCP_MODE[config[CONF_MODE]]
        cg.add(var.set_mcp_mode(mode))

    if CONF_INTERRUPT_PIN in config:
        pin = await cg.gpio_pin_expression(config[CONF_INTERRUPT_PIN])
        cg.add(var.set_interrupt_pin(pin))
    # The reader's SPI path runs from IRAM: no flash-cache misses while draining.
    # IDF only allows the SPI options with FreeRTOS and heap functions in IRAM
    # (ESPHome places both in flash to save IRAM).
    esp32.add_idf_sdkconfig_option("CONFIG_FREERTOS_PLACE_FUNCTIONS_INTO_FLASH", False)
    esp32.add_idf_sdkconfig_option("CONFIG_HEAP_PLACE_FUNCTION_INTO_FLASH", False)
    esp32.add_idf_sdkconfig_option("CONFIG_SPI_MASTER_ISR_IN_IRAM", True)
    esp32.add_idf_sdkconfig_option("CONFIG_SPI_MASTER_IN_IRAM", True)
    cg.add(var.set_rx_queue_len(config[CONF_RX_QUEUE_LEN]))

    await spi.register_spi_device(var, config)
