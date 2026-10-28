#ifndef TEST_HAL_H
#define TEST_HAL_H
#include <stdint.h>
typedef enum { HAL_OK, HAL_ERROR } HAL_StatusTypeDef;
typedef struct { uint32_t OPTCR; } MockFlash;
extern MockFlash mock_flash;
#define FLASH (&mock_flash)
#define FLASH_OPTCR_SPRMOD (1UL << 31)
#define OB_WRP_SECTOR_0 1U
#define OB_WRP_SECTOR_1 2U
#define OPTIONBYTE_WRP 1U
#define OB_WRPSTATE_ENABLE 1U
#define FLASH_BANK_1 1U
#define FLASH_FLAG_EOP 1U
#define FLASH_FLAG_OPERR 2U
#define FLASH_FLAG_WRPERR 4U
#define FLASH_FLAG_PGAERR 8U
#define FLASH_FLAG_PGPERR 16U
#define FLASH_FLAG_PGSERR 32U
#define __HAL_FLASH_CLEAR_FLAG(flags) ((void)(flags))
typedef struct {
    uint32_t OptionType, WRPState, WRPSector, Banks;
    uint32_t RDPLevel, BORLevel, USERConfig;
} FLASH_OBProgramInitTypeDef;
void HAL_FLASHEx_OBGetConfig(FLASH_OBProgramInitTypeDef *options);
HAL_StatusTypeDef HAL_FLASH_Unlock(void);
HAL_StatusTypeDef HAL_FLASH_Lock(void);
HAL_StatusTypeDef HAL_FLASH_OB_Unlock(void);
HAL_StatusTypeDef HAL_FLASH_OB_Lock(void);
HAL_StatusTypeDef HAL_FLASHEx_OBProgram(FLASH_OBProgramInitTypeDef *options);
HAL_StatusTypeDef HAL_FLASH_OB_Launch(void);
#endif
