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

static uint8_t *memory(uint32_t address)
{
    assert(address >= 0x08000000U && address < 0x08080000U);
    return flash_memory + address - 0x08000000U;
}

void Boot_FlashRead(uint32_t address, void *out, size_t size)
{
    assert((uint64_t)address + size <= 0x08080000U);
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

static uint32_t image_crc(void) { return Boot_Crc32(memory(BOOT_APP_ADDR), 256); }

static void fresh(void)
{
    memset(flash_memory, 0xFF, sizeof(flash_memory));
    for (unsigned i = 0; i < 256; ++i) memory(BOOT_APP_ADDR)[i] = (uint8_t)i;
    uint32_t vectors[2] = {0x20020000U, BOOT_APP_ADDR + 0x81U};
    memcpy(memory(BOOT_APP_ADDR), vectors, sizeof(vectors));
    writes_left = -1;
    torn_word = torn_erase = 0;
    erase_count = 0;
}

static void seed(void)
{
    fresh();
    assert(Boot_InstallImage(0x10000, 256, image_crc()) == BOOT_SUCCESS);
}

static void test_states(void)
{
    BootMetadata record;
    fresh();
    assert(Boot_Prepare(0, &record) == BOOT_EMPTY);
    assert(Boot_InstallImage(1, BOOT_APP_MAX_SIZE + 1U, 0) == BOOT_BAD_IMAGE);
    assert(Boot_InstallImage(1, 256, image_crc() ^ 1U) == BOOT_BAD_IMAGE);
    seed();
    for (unsigned i = 1; i <= BOOT_MAX_ATTEMPTS; ++i) {
        assert(Boot_Prepare(1UL << 29, &record) == BOOT_SUCCESS);
        assert(record.boot_state == APP_START && record.boot_attempts == i);
        assert(record.reset_causes == BOOT_RESET_IWDG && record.total_boots == i);
        if (i > 1) assert(record.failure_reason == BOOT_FAILURE_UNCONFIRMED);
    }
    assert(Boot_Prepare(1UL << 28, &record) == BOOT_ATTEMPTS_EXCEEDED);
    assert(record.boot_state == APP_FAIL && record.boot_attempts == 3U);
    assert(Boot_Confirm() == BOOT_BAD_STATE);
    assert(Boot_Prepare(0, &record) == BOOT_ATTEMPTS_EXCEEDED);
    assert(Boot_InstallImage(2, 256, image_crc()) == BOOT_SUCCESS);
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
            assert(record.sequence == 1U && record.boot_state == APP_OK);
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

int main(int argc, char **argv)
{
    test_states();
    test_power_loss(0);
    test_power_loss(1);
    test_crc_and_sequence();
    test_confirm_power_loss();
    puts("PASS: states, image/metadata CRC, reset flags, retries/confirm, sequence wrap,");
    puts("      80 interrupted writes, interrupted erase and journal recovery");
    if (argc == 3) test_provisioned_image(argv[1], argv[2]);
    return 0;
}
