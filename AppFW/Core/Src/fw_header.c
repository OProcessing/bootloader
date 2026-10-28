#include "../../../Shared/firmware_image.h"
/* Post-build finalizer fills version, size and CRC in BOTH ELF and .bin. */
__attribute__((section(".fw_header"), used))
const FirmwareHeader app_firmware_header = {
    FW_HEADER_MAGIC, FW_HEADER_FORMAT, 0U, 0U, 0U, 0U,
    0x08008000U, FW_HEADER_BYTES
};
