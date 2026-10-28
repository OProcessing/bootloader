#include "boot_state.h"
#include <string.h>

#define RECORDS_PER_BANK (BOOT_META_BANK_SIZE / sizeof(BootMetadata))

static uint32_t crc_update(uint32_t crc, const uint8_t *bytes, size_t size)
{
    while (size-- != 0U) {
        crc ^= *bytes++;
        for (unsigned bit = 0; bit < 8U; ++bit) {
            crc = (crc >> 1) ^ ((0U - (crc & 1U)) & UINT32_C(0xEDB88320));
        }
    }
    return crc;
}

uint32_t Boot_Crc32(const void *data, size_t size)
{
    return ~crc_update(UINT32_MAX, (const uint8_t *)data, size);
}

uint32_t Boot_DecodeReset(uint32_t csr)
{
    uint32_t causes = 0U;
    if (csr & (1UL << 27)) causes |= BOOT_RESET_POR;
    if (csr & (1UL << 25)) causes |= BOOT_RESET_BOR;
    if (csr & (1UL << 29)) causes |= BOOT_RESET_IWDG;
    if (csr & (1UL << 30)) causes |= BOOT_RESET_WWDG;
    if (csr & (1UL << 28)) causes |= BOOT_RESET_SOFTWARE;
    if (csr & (1UL << 26)) causes |= BOOT_RESET_PIN;
    if (csr & (1UL << 31)) causes |= BOOT_RESET_LOW_POWER;
    return causes;
}

static uint32_t bank_address(unsigned bank)
{
    return bank == 0U ? BOOT_META_A_ADDR : BOOT_META_B_ADDR;
}

static int erased(const BootMetadata *record)
{
    const uint8_t *bytes = (const uint8_t *)record;
    for (size_t i = 0; i < sizeof(*record); ++i) {
        if (bytes[i] != 0xFFU) return 0;
    }
    return 1;
}

static int valid(const BootMetadata *record)
{
    return record->magic == BOOT_METADATA_MAGIC &&
        record->format_version == BOOT_METADATA_FORMAT &&
        record->commit == BOOT_METADATA_COMMIT &&
        record->boot_state >= APP_START && record->boot_state <= APP_FAIL &&
        record->fw_size >= 8U && record->fw_size <= BOOT_APP_MAX_SIZE &&
        record->boot_attempts <= BOOT_MAX_ATTEMPTS &&
        record->runtime_failures <= BOOT_MAX_RUNTIME_FAILURES &&
        record->record_crc32 == Boot_Crc32(record, offsetof(BootMetadata, record_crc32));
}

/* Full scan skips torn slots. A wrapped sequence is newer within half range. */
static BootResult latest(BootMetadata *out, unsigned *bank_out)
{
    int found = 0;
    int all_erased = 1;
    for (unsigned bank = 0; bank < 2U; ++bank) {
        for (uint32_t slot = 0; slot < RECORDS_PER_BANK; ++slot) {
            BootMetadata record;
            Boot_FlashRead(bank_address(bank) + slot * sizeof(record), &record, sizeof(record));
            if (!erased(&record)) all_erased = 0;
            if (valid(&record)) {
                uint32_t delta = record.sequence - out->sequence;
                if (!found || (delta != 0U && delta < UINT32_C(0x80000000))) {
                    *out = record;
                    *bank_out = bank;
                    found = 1;
                }
            }
        }
    }
    return found ? BOOT_SUCCESS : (all_erased ? BOOT_EMPTY : BOOT_CORRUPT);
}

BootResult Boot_MetadataRead(BootMetadata *out)
{
    unsigned bank;
    if (out == NULL) return BOOT_BAD_STATE;
    memset(out, 0, sizeof(*out));
    return latest(out, &bank);
}

static int free_slot(unsigned bank, uint32_t *address)
{
    for (uint32_t slot = 0; slot < RECORDS_PER_BANK; ++slot) {
        BootMetadata record;
        *address = bank_address(bank) + slot * sizeof(record);
        Boot_FlashRead(*address, &record, sizeof(record));
        if (erased(&record)) return 1;
    }
    return 0;
}

BootResult Boot_MetadataWrite(const BootMetadata *metadata)
{
    BootMetadata previous = {0};
    BootMetadata next;
    BootMetadata verify;
    unsigned active = 0U;
    uint32_t address = 0U;
    int needs_erase = 0;
    if (metadata == NULL) return BOOT_BAD_STATE;
    BootResult status = latest(&previous, &active);
    next = *metadata;
    next.magic = BOOT_METADATA_MAGIC;
    next.format_version = BOOT_METADATA_FORMAT;
    next.sequence = status == BOOT_SUCCESS ? previous.sequence + 1U : 1U;
    next.record_crc32 = Boot_Crc32(&next, offsetof(BootMetadata, record_crc32));
    next.commit = BOOT_METADATA_COMMIT;
    if (!valid(&next)) return BOOT_BAD_STATE;
    /* A corrupt store requires explicit external reprovisioning. */
    if (status == BOOT_CORRUPT) return status;
    if (!free_slot(active, &address)) {
        active ^= 1U;
        address = bank_address(active);
        needs_erase = 1;
    }
    if (!Boot_FlashBegin()) return BOOT_FLASH_ERROR;
    int ok = !needs_erase || Boot_FlashEraseBank(active);
    for (size_t offset = 0; ok && offset < offsetof(BootMetadata, commit); offset += 4U) {
        uint32_t word;
        memcpy(&word, (const uint8_t *)&next + offset, sizeof(word));
        ok = Boot_FlashProgramWord(address + (uint32_t)offset, word);
    }
    /* Verify payload before publishing it with the final commit word. */
    if (ok) {
        Boot_FlashRead(address, &verify, sizeof(verify));
        ok = memcmp(&next, &verify, offsetof(BootMetadata, commit)) == 0;
    }
    if (ok) ok = Boot_FlashProgramWord(address + offsetof(BootMetadata, commit), next.commit);
    Boot_FlashEnd();
    Boot_FlashRead(address, &verify, sizeof(verify));
    return ok && valid(&verify) && memcmp(&next, &verify, sizeof(next)) == 0
        ? BOOT_SUCCESS : BOOT_FLASH_ERROR;
}

static int image_valid(const BootMetadata *record)
{
    uint32_t vectors[2];
    uint8_t buffer[128];
    uint32_t crc = UINT32_MAX;
    if (record->fw_size < 8U || record->fw_size > BOOT_APP_MAX_SIZE) return 0;
    Boot_FlashRead(BOOT_APP_ADDR, vectors, sizeof(vectors));
    uint32_t reset = vectors[1] & ~1UL;
    if (vectors[0] <= UINT32_C(0x20000000) || vectors[0] > UINT32_C(0x20020000) ||
        (vectors[0] & 7U) != 0U || (vectors[1] & 1U) == 0U ||
        reset < BOOT_APP_ADDR || reset >= BOOT_APP_ADDR + record->fw_size) return 0;
    for (uint32_t offset = 0; offset < record->fw_size;) {
        uint32_t size = record->fw_size - offset;
        if (size > sizeof(buffer)) size = sizeof(buffer);
        Boot_FlashRead(BOOT_APP_ADDR + offset, buffer, size);
        crc = crc_update(crc, buffer, size);
        offset += size;
    }
    return ~crc == record->fw_crc32;
}

BootResult Boot_InstallImage(uint32_t version, uint32_t size, uint32_t crc32)
{
    BootMetadata record = {0};
    record.boot_state = APP_OK;
    record.fw_version = version;
    record.fw_size = size;
    record.fw_crc32 = crc32;
    if (!image_valid(&record)) return BOOT_BAD_IMAGE;
    return Boot_MetadataWrite(&record);
}

BootResult Boot_Prepare(uint32_t reset_flags, BootMetadata *out)
{
    BootMetadata record;
    BootResult status = Boot_MetadataRead(&record);
    BootResult decision = BOOT_SUCCESS;
    if (status != BOOT_SUCCESS) return status;
    int had_previous_boot = record.total_boots != 0U;
    record.reset_flags = reset_flags;
    record.reset_causes = Boot_DecodeReset(reset_flags);
    if (record.total_boots != UINT32_MAX) ++record.total_boots;
    if (record.boot_state == APP_FAIL) {
        if (record.failure_reason == BOOT_FAILURE_ATTEMPTS)
            decision = BOOT_ATTEMPTS_EXCEEDED;
        else if (record.failure_reason == BOOT_FAILURE_RUNTIME_LIMIT)
            decision = BOOT_RUNTIME_FAILURES_EXCEEDED;
        else
            decision = BOOT_APP_REJECTED;
    } else if (!image_valid(&record)) {
        record.boot_state = APP_FAIL;
        record.failure_reason = BOOT_FAILURE_IMAGE;
        decision = BOOT_BAD_IMAGE;
    } else {
        uint32_t causes = record.reset_causes;
        int power_reset = (causes & (BOOT_RESET_POR | BOOT_RESET_BOR)) != 0U;
        int watchdog = !power_reset &&
            (causes & (BOOT_RESET_IWDG | BOOT_RESET_WWDG)) != 0U;
        if (record.boot_state == APP_START) {
            record.last_failure = power_reset ? BOOT_FAILURE_POWER_INTERRUPTED :
                (watchdog ? BOOT_FAILURE_STARTUP_WATCHDOG : BOOT_FAILURE_STARTUP_RESET);
            record.failure_reason = record.last_failure;
        } else {
            record.boot_attempts = 0U;
            record.failure_reason = BOOT_FAILURE_NONE;
            if (watchdog && had_previous_boot) {
                if (record.runtime_failures < BOOT_MAX_RUNTIME_FAILURES)
                    ++record.runtime_failures;
                record.last_failure = BOOT_FAILURE_RUNTIME_WATCHDOG;
                record.failure_reason = BOOT_FAILURE_RUNTIME_WATCHDOG;
            }
        }
        if (record.runtime_failures >= BOOT_MAX_RUNTIME_FAILURES) {
            record.boot_state = APP_FAIL;
            record.failure_reason = BOOT_FAILURE_RUNTIME_LIMIT;
            decision = BOOT_RUNTIME_FAILURES_EXCEEDED;
        } else if (record.boot_attempts >= BOOT_MAX_ATTEMPTS) {
            record.boot_state = APP_FAIL;
            record.failure_reason = BOOT_FAILURE_ATTEMPTS;
            decision = BOOT_ATTEMPTS_EXCEEDED;
        } else {
            record.boot_state = APP_START;
            ++record.boot_attempts;
        }
    }
    status = Boot_MetadataWrite(&record);
    if (status != BOOT_SUCCESS) return status;
    status = Boot_MetadataRead(&record);
    if (status != BOOT_SUCCESS) return status;
    if (out != NULL) *out = record;
    return decision;
}

BootResult Boot_Confirm(void)
{
    BootMetadata record;
    BootResult status = Boot_MetadataRead(&record);
    if (status != BOOT_SUCCESS) return status;
    if (record.boot_state == APP_OK) return BOOT_SUCCESS;
    if (record.boot_state != APP_START) return BOOT_BAD_STATE;
    if (!image_valid(&record)) return BOOT_BAD_IMAGE;
    record.boot_state = APP_OK;
    record.boot_attempts = 0U;
    record.failure_reason = BOOT_FAILURE_NONE;
    return Boot_MetadataWrite(&record);
}

BootResult Boot_MarkFailed(void)
{
    BootMetadata record;
    BootResult status = Boot_MetadataRead(&record);
    if (status != BOOT_SUCCESS) return status;
    record.boot_state = APP_FAIL;
    record.failure_reason = BOOT_FAILURE_APPLICATION;
    record.last_failure = BOOT_FAILURE_APPLICATION;
    return Boot_MetadataWrite(&record);
}

BootResult Boot_RuntimeHealthy(void)
{
    BootMetadata record;
    BootResult status = Boot_MetadataRead(&record);
    if (status != BOOT_SUCCESS) return status;
    if (record.boot_state != APP_OK) return BOOT_BAD_STATE;
    if (record.runtime_failures == 0U) return BOOT_SUCCESS;
    record.runtime_failures = 0U;
    return Boot_MetadataWrite(&record);
}
