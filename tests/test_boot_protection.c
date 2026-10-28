#include "protection_mock/stm32f4xx_hal.h"
#include "../Shared/boot_protection.h"
#include <assert.h>
#include <stdio.h>

MockFlash mock_flash;
static unsigned unlocks, ob_unlocks, locks, ob_locks, programs, launches;
static unsigned fail;
static FLASH_OBProgramInitTypeDef stored;
void HAL_FLASHEx_OBGetConfig(FLASH_OBProgramInitTypeDef *out) { *out = stored; }
HAL_StatusTypeDef HAL_FLASH_Unlock(void) { ++unlocks; return fail == 1 ? HAL_ERROR : HAL_OK; }
HAL_StatusTypeDef HAL_FLASH_OB_Unlock(void) { ++ob_unlocks; return fail == 2 ? HAL_ERROR : HAL_OK; }
HAL_StatusTypeDef HAL_FLASH_Lock(void) { ++locks; return HAL_OK; }
HAL_StatusTypeDef HAL_FLASH_OB_Lock(void) { ++ob_locks; return HAL_OK; }
HAL_StatusTypeDef HAL_FLASHEx_OBProgram(FLASH_OBProgramInitTypeDef *options)
{
    ++programs;
    assert(options->OptionType == OPTIONBYTE_WRP && options->WRPSector == 3U);
    assert(options->WRPState == OB_WRPSTATE_ENABLE && options->Banks == FLASH_BANK_1);
    assert(options->RDPLevel == 0xAA && options->BORLevel == 0xCU && options->USERConfig == 0xE0U);
    if (fail == 3) return HAL_ERROR;
    stored.WRPSector &= ~options->WRPSector;
    return HAL_OK;
}
HAL_StatusTypeDef HAL_FLASH_OB_Launch(void) { ++launches; return fail == 4 ? HAL_ERROR : HAL_OK; }
static void reset(void)
{
    stored = (FLASH_OBProgramInitTypeDef){0};
    stored.WRPSector = 0xFF;
    stored.RDPLevel = 0xAA;
    stored.BORLevel = 0xC;
    stored.USERConfig = 0xE0;
    mock_flash.OPTCR = 0;
    unlocks = ob_unlocks = locks = ob_locks = programs = launches = fail = 0;
}
int main(void)
{
    reset();
    assert(Boot_ProtectBootloader() == BOOT_PROTECTION_POWER_CYCLE_REQUIRED);
    assert(stored.WRPSector == 0xFC && locks == 1 && ob_locks == 1);
    assert(Boot_ProtectBootloader() == BOOT_PROTECTION_OK && programs == 1 && launches == 1);
    reset();
    mock_flash.OPTCR = FLASH_OPTCR_SPRMOD;
    assert(Boot_ProtectBootloader() == BOOT_PROTECTION_PCROP_MODE && unlocks == 0);
    for (unsigned error = 1; error <= 4; ++error) {
        reset();
        fail = error;
        assert(Boot_ProtectBootloader() == BOOT_PROTECTION_ERROR);
        assert(locks == (error == 1 ? 0U : 1U));
        assert(ob_locks == (error <= 2 ? 0U : 1U));
    }
    puts("PASS: WRP sectors 0/1 only, preserves options, PCROP refusal, lock/error handling");
    return 0;
}
