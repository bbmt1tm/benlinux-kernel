idf_component_register(
    SRCS "probe_main.c" "probe_trap.S"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_mm esp_hw_support hal soc riscv heap esp_rom freertos
)
