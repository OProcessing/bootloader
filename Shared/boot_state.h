#ifndef BOOT_STATE_H
#define BOOT_STATE_H

#include <stddef.h>
#include <stdint.h>
#include "firmware_image.h"

#define BOOT_APP_ADDR          UINT32_C(0x08010000)
#define BOOT_APP_END           UINT32_C(0x08080000)
#define BOOT_APP_MAX_SIZE      (BOOT_APP_END - BOOT_APP_ADDR)
#define BOOT_META_A_ADDR       UINT32_C(0x08008000)
#define BOOT_META_B_ADDR       UINT32_C(0x0800C000)
#define BOOT_META_BANK_SIZE    UINT32_C(0x00004000)
#define BOOT_MAX_ATTEMPTS      3U
#define BOOT_MAX_RUNTIME_FAILURES 3U
#define BOOT_METADATA_MAGIC    UINT32_C(0x424F4F54)
#define BOOT_METADATA_FORMAT   2U
#define BOOT_CRC_CHECK_INTERVAL 16U
#define BOOT_METADATA_COMMIT   UINT32_C(0x434F4D54)

_Static_assert(BOOT_META_A_ADDR + BOOT_META_BANK_SIZE == BOOT_META_B_ADDR,
               "Metadata banks must be contiguous");
_Static_assert(BOOT_META_B_ADDR + BOOT_META_BANK_SIZE == BOOT_APP_ADDR,
               "Metadata must end at the application boundary");

typedef enum {
    APP_START = 1, APP_OK = 2, APP_FAIL = 3, APP_READY = 4
} BootState;

/* Bit mask: reset flags can be asserted simultaneously. */
enum {
    BOOT_RESET_POR = 1U << 0, BOOT_RESET_BOR = 1U << 1,
    BOOT_RESET_IWDG = 1U << 2, BOOT_RESET_WWDG = 1U << 3,
    BOOT_RESET_SOFTWARE = 1U << 4, BOOT_RESET_PIN = 1U << 5,
    BOOT_RESET_LOW_POWER = 1U << 6
};

enum {
    BOOT_FAILURE_NONE = 0, BOOT_FAILURE_UNCONFIRMED = 1,
    BOOT_FAILURE_ATTEMPTS = 2, BOOT_FAILURE_IMAGE = 3,
    BOOT_FAILURE_APPLICATION = 4,
    BOOT_FAILURE_STARTUP_WATCHDOG = 5,
    BOOT_FAILURE_RUNTIME_WATCHDOG = 6,
    BOOT_FAILURE_POWER_INTERRUPTED = 7,
    BOOT_FAILURE_STARTUP_RESET = 8,
    BOOT_FAILURE_RUNTIME_LIMIT = 9
};

typedef enum {
    BOOT_SUCCESS = 0, BOOT_EMPTY, BOOT_CORRUPT, BOOT_FLASH_ERROR,
    BOOT_BAD_IMAGE, BOOT_ATTEMPTS_EXCEEDED, BOOT_APP_REJECTED,
    BOOT_BAD_STATE, BOOT_RUNTIME_FAILURES_EXCEEDED, BOOT_WATCHDOG_ERROR
} BootResult;

/* On-flash ABI: 16 little-endian uint32 words, CRC over first 56 bytes.
 * commit is programmed LAST. Both projects must use this same definition. */
typedef struct {
    uint32_t magic;
    uint32_t format_version;
    uint32_t sequence;
    uint32_t boot_state;
    uint32_t fw_version;
    uint32_t fw_crc32;
    uint32_t fw_size;
    uint32_t boot_attempts;
    uint32_t total_boots;
    uint32_t reset_flags;
    uint32_t reset_causes;
    uint32_t failure_reason;
    /* Former reserved words (zero in stage 2), preserving the 64-byte ABI. */
    uint32_t runtime_failures;
    uint32_t last_failure;
    uint32_t record_crc32;
    uint32_t commit;
} BootMetadata;

_Static_assert(sizeof(BootMetadata) == 64U, "Metadata ABI size");
_Static_assert(offsetof(BootMetadata, record_crc32) == 56U, "Metadata CRC ABI");

uint32_t Boot_Crc32(const void *data, size_t size);
uint32_t Boot_DecodeReset(uint32_t rcc_csr);
BootResult Boot_MetadataRead(BootMetadata *out);
/* Assigns sequence, magic, CRC and commit; callers must serialize writes. */
BootResult Boot_MetadataWrite(const BootMetadata *metadata);
/* Explicit installation API; does not clear records automatically on errors. */
BootResult Boot_InstallImage(uint32_t version, uint32_t size, uint32_t crc32);
BootResult Boot_Prepare(uint32_t reset_flags, BootMetadata *out);
BootResult Boot_PrepareEx(uint32_t reset_flags, int force_crc, BootMetadata *out);
/* Always checks header, vectors and FULL image CRC against metadata. */
BootResult Boot_VerifyFirmware(void);
BootResult Boot_Confirm(void);
/* Call only after a sustained healthy runtime interval, not at boot confirm. */
BootResult Boot_RuntimeHealthy(void);
BootResult Boot_MarkFailed(void);

/* Backend contract. Never call from an ISR or concurrently. */
void Boot_FlashRead(uint32_t address, void *out, size_t size);
int Boot_FlashBegin(void);
void Boot_FlashEnd(void);
int Boot_FlashProgramWord(uint32_t address, uint32_t word);
int Boot_FlashEraseBank(unsigned bank);

#endif
