/* STM32F446RE, 3.3 V supply (FLASH_VOLTAGE_RANGE_3 requires 2.7-3.6 V).
 * Flash is single-bank: instruction fetch stalls during program/erase.
 * Call in thread mode with SysTick running; HAL uses it for timeout handling.
 * No ISR may write Flash or access this module concurrently. */
#include "boot_state.h"
#include "stm32f4xx_hal.h"
#include <string.h>

void Boot_FlashRead(uint32_t address, void *out, size_t size)
{
    memcpy(out, (const void *)(uintptr_t)address, size);
}

int Boot_FlashBegin(void)
{
    if (__get_IPSR() != 0U || __get_PRIMASK() != 0U) return 0;
    if (HAL_FLASH_Unlock() != HAL_OK) return 0;
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_OPERR | FLASH_FLAG_WRPERR |
                          FLASH_FLAG_PGAERR | FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR);
    return 1;
}

void Boot_FlashEnd(void)
{
    HAL_FLASH_Lock();
}

static void refresh_cache(void)
{
    /* Avoid stale reads after erase/program, preserving cache enable state. */
    uint32_t saved = FLASH->ACR & (FLASH_ACR_ICEN | FLASH_ACR_DCEN);
    __HAL_FLASH_INSTRUCTION_CACHE_DISABLE();
    __HAL_FLASH_DATA_CACHE_DISABLE();
    __HAL_FLASH_INSTRUCTION_CACHE_RESET();
    __HAL_FLASH_DATA_CACHE_RESET();
    FLASH->ACR |= saved;
    __DSB();
    __ISB();
}

int Boot_FlashProgramWord(uint32_t address, uint32_t word)
{
    if (address < BOOT_META_A_ADDR || address > BOOT_META_B_ADDR + BOOT_META_BANK_SIZE - 4U ||
        (address & 3U) != 0U) return 0;
    HAL_StatusTypeDef status = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, address, word);
    refresh_cache();
    return status == HAL_OK;
}

int Boot_FlashEraseBank(unsigned bank)
{
    FLASH_EraseInitTypeDef erase = {0};
    uint32_t error;
    if (bank > 1U) return 0;
    erase.TypeErase = FLASH_TYPEERASE_SECTORS;
    erase.Sector = bank == 0U ? FLASH_SECTOR_6 : FLASH_SECTOR_7;
    erase.NbSectors = 1U;
    erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;
    HAL_StatusTypeDef status = HAL_FLASHEx_Erase(&erase, &error);
    refresh_cache();
    return status == HAL_OK;
}
