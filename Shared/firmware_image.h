#ifndef FIRMWARE_IMAGE_H
#define FIRMWARE_IMAGE_H
#include <stdint.h>
#define FW_HEADER_OFFSET 0x200U
#define FW_HEADER_MAGIC UINT32_C(0x4657494D)
#define FW_HEADER_FORMAT 1U
#define FW_HEADER_BYTES 32U
#define FW_MIN_IMAGE_SIZE (FW_HEADER_OFFSET + FW_HEADER_BYTES)
/* Image CRC covers the entire image with both CRC words treated as zero.
 * Header CRC covers this structure with only header_crc32 treated as zero. */
typedef struct {
    uint32_t magic;
    uint32_t format_version;
    uint32_t image_size;
    uint32_t fw_version;
    uint32_t image_crc32;
    uint32_t header_crc32;
    uint32_t load_address;
    uint32_t header_size;
} FirmwareHeader;
_Static_assert(sizeof(FirmwareHeader) == FW_HEADER_BYTES, "FW header ABI");
#endif
