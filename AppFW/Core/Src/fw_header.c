#include "../../../Shared/boot_state.h"
/* Post-build finalizer fills version, size and CRC in BOTH ELF and .bin. */
__attribute__((section(".fw_header"), used))
const FirmwareHeader app_firmware_header = {
    FW_HEADER_MAGIC, FW_HEADER_FORMAT, 0U, 0U, 0U, 0U,
    BOOT_APP_ADDR, FW_HEADER_BYTES
};
