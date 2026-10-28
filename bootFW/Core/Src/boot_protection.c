#include "../../../Shared/boot_protection.h"
#include "stm32f4xx_hal.h"

BootProtectionResult Boot_ProtectBootloader(void)
{
    const uint32_t sectors = OB_WRP_SECTOR_0 | OB_WRP_SECTOR_1;
    FLASH_OBProgramInitTypeDef options = {0};
    if ((FLASH->OPTCR & FLASH_OPTCR_SPRMOD) != 0U)
        return BOOT_PROTECTION_PCROP_MODE;
    HAL_FLASHEx_OBGetConfig(&options);
    /* WRP is active-low; GetConfig returns the raw nWRP mask. */
    if ((options.WRPSector & sectors) == 0U) return BOOT_PROTECTION_OK;
    if (HAL_FLASH_Unlock() != HAL_OK) return BOOT_PROTECTION_ERROR;
    if (HAL_FLASH_OB_Unlock() != HAL_OK) {
        HAL_FLASH_Lock();
        return BOOT_PROTECTION_ERROR;
    }
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_OPERR | FLASH_FLAG_WRPERR |
                          FLASH_FLAG_PGAERR | FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR);
    options.OptionType = OPTIONBYTE_WRP;
    options.WRPState = OB_WRPSTATE_ENABLE;
    options.WRPSector = sectors;
    options.Banks = FLASH_BANK_1;
    HAL_StatusTypeDef status = HAL_FLASHEx_OBProgram(&options);
    /* On F446 HAL_OB_Launch commits OPTSTRT; it is NOT an automatic reset. */
    if (status == HAL_OK) status = HAL_FLASH_OB_Launch();
    HAL_FLASH_OB_Lock();
    HAL_FLASH_Lock();
    return status == HAL_OK ? BOOT_PROTECTION_POWER_CYCLE_REQUIRED : BOOT_PROTECTION_ERROR;
}
