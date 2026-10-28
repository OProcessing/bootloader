#include "../Shared/boot_state.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t flash_memory[512U * 1024U];
static uint8_t saved_flash[sizeof(flash_memory)];
static int writes_left = -1;
static int torn_word;
static int torn_erase;
static unsigned erase_count;
static unsigned image_bulk_reads;

static uint8_t *memory(uint32_t address)
{
    assert(address >= 0x08000000U && address < 0x08080000U);
    return flash_memory + address - 0x08000000U;
}

void Boot_FlashRead(uint32_t address, void *out, size_t size)
{
    assert((uint64_t)address + size <= 0x08080000U);
    if (address >= BOOT_APP_ADDR && address < BOOT_APP_END && size >= 128U)
        ++image_bulk_reads;
    memcpy(out, memory(address), size);
}
int Boot_FlashBegin(void) { return 1; }
void Boot_FlashEnd(void) { }
int Boot_FlashProgramWord(uint32_t address, uint32_t word)
{
    assert(address >= BOOT_META_A_ADDR && (address & 3U) == 0U);
    uint32_t old;
    memcpy(&old, memory(address), 4);
    assert((old & word) == word);
    if (writes_left == 0) {
        if (torn_word) {
            uint32_t partial = word | 0xFFFF0000U;
            memcpy(memory(address), &partial, 4);
        }
        return 0;
    }
    if (writes_left > 0) --writes_left;
    memcpy(memory(address), &word, 4);
    return 1;
}
int Boot_FlashEraseBank(unsigned bank)
{
    assert(bank < 2U);
    ++erase_count;
    memset(memory(bank ? BOOT_META_B_ADDR : BOOT_META_A_ADDR), 0xFF,
           torn_erase ? BOOT_META_BANK_SIZE / 2U : BOOT_META_BANK_SIZE);
    return !torn_erase;
}

static uint32_t image_crc(void)
{
    FirmwareHeader header;
    memcpy(&header, memory(BOOT_APP_ADDR + FW_HEADER_OFFSET), sizeof(header));
    return header.image_crc32;
}
static void set_image_version(uint32_t version)
{
    FirmwareHeader header = {FW_HEADER_MAGIC, FW_HEADER_FORMAT, 768, version, 0, 0,
                             BOOT_APP_ADDR, sizeof(FirmwareHeader)};
    memcpy(memory(BOOT_APP_ADDR + FW_HEADER_OFFSET), &header, sizeof(header));
    header.image_crc32 = Boot_Crc32(memory(BOOT_APP_ADDR), 768);
    header.header_crc32 = Boot_Crc32(&header, sizeof(header));
    memcpy(memory(BOOT_APP_ADDR + FW_HEADER_OFFSET), &header, sizeof(header));
}

static void fresh(void)
{
    memset(flash_memory, 0xFF, sizeof(flash_memory));
    for (unsigned i = 0; i < 768; ++i) memory(BOOT_APP_ADDR)[i] = (uint8_t)i;
    uint32_t vectors[2] = {0x20020000U, BOOT_APP_ADDR + 0x281U};
    memcpy(memory(BOOT_APP_ADDR), vectors, sizeof(vectors));
    set_image_version(0x10000U);
    writes_left = -1;
    torn_word = torn_erase = 0;
    erase_count = 0;
}

static void seed(void)
{
    fresh();
    assert(Boot_InstallImage(0x10000, 768, image_crc()) == BOOT_SUCCESS);
}

static void test_states(void)
{
    BootMetadata record;
    fresh();
    assert(Boot_Prepare(0, &record) == BOOT_EMPTY);
    assert(Boot_InstallImage(1, BOOT_APP_MAX_SIZE + 1U, 0) == BOOT_BAD_IMAGE);
    assert(Boot_InstallImage(1, 768, image_crc() ^ 1U) == BOOT_BAD_IMAGE);
    seed();
    for (unsigned i = 1; i <= BOOT_MAX_ATTEMPTS; ++i) {
        assert(Boot_Prepare(1UL << 29, &record) == BOOT_SUCCESS);
        assert(record.boot_state == APP_START && record.boot_attempts == i);
        assert(record.reset_causes == BOOT_RESET_IWDG && record.total_boots == i);
        if (i > 1) assert(record.failure_reason == BOOT_FAILURE_STARTUP_WATCHDOG);
    }
    assert(Boot_Prepare(1UL << 28, &record) == BOOT_ATTEMPTS_EXCEEDED);
    assert(record.boot_state == APP_FAIL && record.boot_attempts == 3U);
    assert(Boot_Confirm() == BOOT_BAD_STATE);
    assert(Boot_Prepare(0, &record) == BOOT_ATTEMPTS_EXCEEDED);
    set_image_version(2);
    assert(Boot_InstallImage(2, 768, image_crc()) == BOOT_SUCCESS);
    assert(Boot_Prepare(0, &record) == BOOT_SUCCESS);
    assert(Boot_Confirm() == BOOT_SUCCESS);
    assert(Boot_MetadataRead(&record) == BOOT_SUCCESS);
    uint32_t sequence = record.sequence;
    assert(record.boot_state == APP_OK && record.boot_attempts == 0);
    assert(Boot_Confirm() == BOOT_SUCCESS);
    assert(Boot_MetadataRead(&record) == BOOT_SUCCESS && record.sequence == sequence);
    assert(Boot_Prepare(0, &record) == BOOT_SUCCESS && record.boot_attempts == 1U);
    assert(Boot_MarkFailed() == BOOT_SUCCESS);
    assert(Boot_Prepare(0, &record) == BOOT_APP_REJECTED);
    seed();
    memory(BOOT_APP_ADDR)[100] ^= 1U;
    assert(Boot_Prepare(0, &record) == BOOT_BAD_IMAGE);
    assert(record.boot_state == APP_FAIL && record.failure_reason == BOOT_FAILURE_IMAGE);
    seed();
    memory(BOOT_APP_ADDR)[4] &= 0xFEU;
    assert(Boot_Prepare(0, &record) == BOOT_BAD_IMAGE);
    assert(erase_count == 0U);
}

static void test_power_loss(int rotate)
{
    BootMetadata record;
    seed();
    if (rotate) {
        /* Occupied/corrupt slots simulate a full journal; retain its first valid record. */
        memset(memory(BOOT_META_A_ADDR) + 64, 0, BOOT_META_BANK_SIZE - 64);
        memset(memory(BOOT_META_B_ADDR), 0, BOOT_META_BANK_SIZE);
    }
    memcpy(saved_flash, flash_memory, sizeof(flash_memory));
    for (int partial = 0; partial < 2; ++partial) {
        for (int cut = 0; cut < 16; ++cut) {
            memcpy(flash_memory, saved_flash, sizeof(flash_memory));
            writes_left = cut;
            torn_word = partial;
            assert(Boot_Prepare(0, &record) == BOOT_FLASH_ERROR);
            /* Simulated reboot must still find the previous committed record. */
            assert(Boot_MetadataRead(&record) == BOOT_SUCCESS);
            assert(record.sequence == 1U && record.boot_state == APP_READY);
            writes_left = -1;
            torn_word = 0;
            assert(Boot_Prepare(0, &record) == BOOT_SUCCESS);
            assert(record.sequence == 2U && record.boot_attempts == 1U);
        }
    }
    if (rotate) {
        memcpy(flash_memory, saved_flash, sizeof(flash_memory));
        torn_erase = 1;
        assert(Boot_Prepare(0, &record) == BOOT_FLASH_ERROR);
        assert(Boot_MetadataRead(&record) == BOOT_SUCCESS && record.sequence == 1U);
        torn_erase = 0;
        assert(Boot_Prepare(0, &record) == BOOT_SUCCESS);
    }
}

static void test_crc_and_sequence(void)
{
    BootMetadata record;
    assert(Boot_Crc32("123456789", 9) == 0xCBF43926U);
    seed();
    assert(Boot_Prepare(0, &record) == BOOT_SUCCESS);
    memory(BOOT_META_A_ADDR)[64 + 16] ^= 1U;
    assert(Boot_MetadataRead(&record) == BOOT_SUCCESS && record.sequence == 1U);
    memory(BOOT_META_A_ADDR)[16] ^= 1U;
    assert(Boot_MetadataRead(&record) == BOOT_CORRUPT);
    assert(Boot_Prepare(0, &record) == BOOT_CORRUPT);
    seed();
    assert(Boot_MetadataRead(&record) == BOOT_SUCCESS);
    record.sequence = UINT32_MAX;
    record.record_crc32 = Boot_Crc32(&record, offsetof(BootMetadata, record_crc32));
    memcpy(memory(BOOT_META_A_ADDR), &record, sizeof(record));
    assert(Boot_Prepare(0, &record) == BOOT_SUCCESS && record.sequence == 0U);
    assert(Boot_Confirm() == BOOT_SUCCESS);
    assert(Boot_MetadataRead(&record) == BOOT_SUCCESS && record.sequence == 1U);
    uint32_t flags[] = {1UL << 27, 1UL << 25, 1UL << 29, 1UL << 30,
                        1UL << 28, 1UL << 26, 1UL << 31};
    for (unsigned i = 0; i < 7U; ++i) assert(Boot_DecodeReset(flags[i]) == (1U << i));
    assert(Boot_DecodeReset(0xFE000000U) == 0x7FU);
}

static void test_confirm_power_loss(void)
{
    BootMetadata record;
    seed();
    assert(Boot_Prepare(0, &record) == BOOT_SUCCESS);
    memcpy(saved_flash, flash_memory, sizeof(flash_memory));
    for (int cut = 0; cut < 16; ++cut) {
        memcpy(flash_memory, saved_flash, sizeof(flash_memory));
        writes_left = cut;
        torn_word = 1;
        assert(Boot_Confirm() == BOOT_FLASH_ERROR);
        assert(Boot_MetadataRead(&record) == BOOT_SUCCESS);
        assert(record.boot_state == APP_START && record.boot_attempts == 1U);
        writes_left = -1;
        torn_word = 0;
        assert(Boot_Prepare(0, &record) == BOOT_SUCCESS && record.boot_attempts == 2U);
    }
}

static void test_provisioned_image(const char *image_path, const char *metadata_path)
{
    BootMetadata record;
    fresh();
    FILE *image = fopen(image_path, "rb");
    assert(image != NULL);
    size_t size = fread(memory(BOOT_APP_ADDR), 1, BOOT_APP_MAX_SIZE, image);
    assert(feof(image));
    fclose(image);
    FILE *metadata = fopen(metadata_path, "rb");
    assert(metadata != NULL);
    assert(fread(memory(BOOT_META_A_ADDR), 1, 64, metadata) == 64U);
    fclose(metadata);
    assert(Boot_MetadataRead(&record) == BOOT_SUCCESS && record.fw_size == size);
    assert(Boot_Prepare(1UL << 27, &record) == BOOT_SUCCESS);
    assert(Boot_Confirm() == BOOT_SUCCESS);
    puts("PASS: actual ARM application .bin + Python-generated metadata + C boot/confirm");
}

static void test_stage3_reset_policy(void)
{
    BootMetadata record;
    const uint32_t power = 1UL << 27;
    const uint32_t iwdg = 1UL << 29;
    const uint32_t wwdg = 1UL << 30;
    const uint32_t software = 1UL << 28;
    seed();
    assert(Boot_Prepare(power, &record) == BOOT_SUCCESS);
    assert(Boot_RuntimeHealthy() == BOOT_BAD_STATE);
    assert(Boot_Prepare(iwdg, &record) == BOOT_SUCCESS);
    assert(record.last_failure == BOOT_FAILURE_STARTUP_WATCHDOG);
    assert(record.runtime_failures == 0U && record.boot_attempts == 2U);
    assert(Boot_Confirm() == BOOT_SUCCESS);
    for (unsigned failure = 1; failure <= BOOT_MAX_RUNTIME_FAILURES; ++failure) {
        BootResult result = Boot_Prepare(failure == 2U ? wwdg : iwdg, &record);
        assert(record.runtime_failures == failure);
        assert(record.last_failure == BOOT_FAILURE_RUNTIME_WATCHDOG);
        if (failure < BOOT_MAX_RUNTIME_FAILURES) {
            assert(result == BOOT_SUCCESS && record.boot_attempts == 1U);
            assert(Boot_Confirm() == BOOT_SUCCESS);
            assert(Boot_MetadataRead(&record) == BOOT_SUCCESS);
            assert(record.boot_attempts == 0U && record.runtime_failures == failure);
        } else {
            assert(result == BOOT_RUNTIME_FAILURES_EXCEEDED && record.boot_state == APP_FAIL);
            assert(record.failure_reason == BOOT_FAILURE_RUNTIME_LIMIT);
            assert(Boot_Confirm() == BOOT_BAD_STATE);
            assert(Boot_RuntimeHealthy() == BOOT_BAD_STATE);
            assert(Boot_Prepare(power, &record) == BOOT_RUNTIME_FAILURES_EXCEEDED);
        }
    }
    /* Stable runtime clears its own counter, with no redundant Flash write. */
    seed();
    assert(Boot_Prepare(power, &record) == BOOT_SUCCESS);
    assert(Boot_Confirm() == BOOT_SUCCESS);
    assert(Boot_Prepare(iwdg, &record) == BOOT_SUCCESS && record.runtime_failures == 1U);
    assert(Boot_Confirm() == BOOT_SUCCESS);
    assert(Boot_RuntimeHealthy() == BOOT_SUCCESS);
    assert(Boot_MetadataRead(&record) == BOOT_SUCCESS && record.runtime_failures == 0U);
    uint32_t sequence = record.sequence;
    assert(Boot_RuntimeHealthy() == BOOT_SUCCESS);
    assert(Boot_MetadataRead(&record) == BOOT_SUCCESS && record.sequence == sequence);
    assert(record.last_failure == BOOT_FAILURE_RUNTIME_WATCHDOG);
    /* Normal confirmed resets do not add runtime faults. Mixed power/watchdog
     * flags are conservatively classified as a power event. */
    assert(Boot_Prepare(power | iwdg, &record) == BOOT_SUCCESS && record.runtime_failures == 0U);
    assert(Boot_Prepare(power | iwdg, &record) == BOOT_SUCCESS);
    assert(record.last_failure == BOOT_FAILURE_POWER_INTERRUPTED);
    assert(Boot_Confirm() == BOOT_SUCCESS);
    assert(Boot_Prepare(software, &record) == BOOT_SUCCESS && record.runtime_failures == 0U);
    assert(Boot_Prepare(software, &record) == BOOT_SUCCESS);
    assert(record.last_failure == BOOT_FAILURE_STARTUP_RESET);
    assert(Boot_Prepare(power, &record) == BOOT_SUCCESS && record.boot_attempts == 3U);
    assert(Boot_Prepare(power, &record) == BOOT_ATTEMPTS_EXCEEDED);
    /* A torn healthy-runtime update must NOT silently clear the streak. */
    seed();
    assert(Boot_Prepare(power, &record) == BOOT_SUCCESS);
    assert(Boot_Confirm() == BOOT_SUCCESS);
    assert(Boot_Prepare(iwdg, &record) == BOOT_SUCCESS);
    assert(Boot_Confirm() == BOOT_SUCCESS);
    memcpy(saved_flash, flash_memory, sizeof(flash_memory));
    for (int cut = 0; cut < 16; ++cut) {
        memcpy(flash_memory, saved_flash, sizeof(flash_memory));
        writes_left = cut;
        torn_word = 1;
        assert(Boot_RuntimeHealthy() == BOOT_FLASH_ERROR);
        assert(Boot_MetadataRead(&record) == BOOT_SUCCESS && record.runtime_failures == 1U);
        writes_left = -1;
        torn_word = 0;
        assert(Boot_Prepare(iwdg, &record) == BOOT_SUCCESS && record.runtime_failures == 2U);
    }
    puts("PASS: startup/runtime reset classification, independent limits, stable-runtime reset,");
    puts("      power/software/watchdog combinations, 16 interrupted runtime-healthy records");
}

static void test_stage4_integrity(void)
{
    BootMetadata record;
    seed();
    image_bulk_reads = 0;
    assert(Boot_VerifyFirmware() == BOOT_SUCCESS && image_bulk_reads != 0U);
    image_bulk_reads = 0;
    assert(Boot_Prepare(1UL << 28, &record) == BOOT_SUCCESS && image_bulk_reads != 0U);
    image_bulk_reads = 0;
    assert(Boot_Confirm() == BOOT_SUCCESS && image_bulk_reads == 0U);
    memcpy(saved_flash, flash_memory, sizeof(flash_memory));
    const uint32_t flags[] = {1UL << 28, 1UL << 26, 1UL << 27, 1UL << 25,
                             1UL << 29, 1UL << 30, 0U, 1UL << 31};
    for (unsigned i = 0; i < sizeof(flags) / sizeof(flags[0]); ++i) {
        memcpy(flash_memory, saved_flash, sizeof(flash_memory));
        image_bulk_reads = 0;
        assert(Boot_Prepare(flags[i], &record) == BOOT_SUCCESS);
        assert((image_bulk_reads != 0U) == (i >= 2U));
    }
    memcpy(flash_memory, saved_flash, sizeof(flash_memory));
    image_bulk_reads = 0;
    assert(Boot_PrepareEx(1UL << 28, 1, &record) == BOOT_SUCCESS && image_bulk_reads != 0U);
    memcpy(flash_memory, saved_flash, sizeof(flash_memory));
    assert(Boot_MetadataRead(&record) == BOOT_SUCCESS);
    record.total_boots = BOOT_CRC_CHECK_INTERVAL;
    assert(Boot_MetadataWrite(&record) == BOOT_SUCCESS);
    image_bulk_reads = 0;
    assert(Boot_Prepare(1UL << 28, &record) == BOOT_SUCCESS && image_bulk_reads != 0U);
    /* Skip policy intentionally cannot detect body-only damage on a warm reset. */
    memcpy(flash_memory, saved_flash, sizeof(flash_memory));
    memory(BOOT_APP_ADDR)[700] ^= 1U;
    image_bulk_reads = 0;
    assert(Boot_Prepare(1UL << 28, &record) == BOOT_SUCCESS && image_bulk_reads == 0U);
    assert(Boot_VerifyFirmware() == BOOT_BAD_IMAGE);
    memcpy(flash_memory, saved_flash, sizeof(flash_memory));
    memory(BOOT_APP_ADDR)[700] ^= 1U;
    assert(Boot_Prepare(1UL << 27, &record) == BOOT_BAD_IMAGE && record.boot_state == APP_FAIL);
    /* Header CRC/magic and metadata identity are always checked, even if body is skipped. */
    for (unsigned offset = 0; offset < sizeof(FirmwareHeader); ++offset) {
        memcpy(flash_memory, saved_flash, sizeof(flash_memory));
        memory(BOOT_APP_ADDR + FW_HEADER_OFFSET)[offset] ^= 1U;
        assert(Boot_Prepare(1UL << 28, &record) == BOOT_BAD_IMAGE);
    }
    fresh();
    memory(BOOT_APP_ADDR)[700] ^= 1U;
    assert(Boot_InstallImage(0x10000, 768, image_crc()) == BOOT_BAD_IMAGE);
    assert(Boot_MetadataRead(&record) == BOOT_EMPTY);
    seed();
    assert(Boot_MetadataRead(&record) == BOOT_SUCCESS);
    record.fw_version ^= 1U;
    assert(Boot_MetadataWrite(&record) == BOOT_SUCCESS);
    assert(Boot_Prepare(0, &record) == BOOT_BAD_IMAGE);
    puts("PASS: stage-4 header corruption/identity, install verification, conditional/forced/periodic CRC");
}

int main(int argc, char **argv)
{
    test_states();
    test_power_loss(0);
    test_power_loss(1);
    test_crc_and_sequence();
    test_confirm_power_loss();
    test_stage3_reset_policy();
    test_stage4_integrity();
    puts("PASS: states, image/metadata CRC, reset flags, retries/confirm, sequence wrap,");
    puts("      80 interrupted writes, interrupted erase and journal recovery");
    if (argc == 3) test_provisioned_image(argv[1], argv[2]);
    return 0;
}
