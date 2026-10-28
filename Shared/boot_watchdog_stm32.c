#include "boot_watchdog.h"
#include "stm32f4xx.h"

int Boot_WatchdogStart(void)
{
    /* Starting IWDG forces LSI on. It cannot be stopped until system reset. */
    IWDG->KR = 0xCCCCU;
    IWDG->KR = 0x5555U;
    IWDG->PR = 6U;       /* /256 */
    IWDG->RLR = 4095U;
    /* Bounded wait works before HAL/SysTick initialization and after jump. */
    uint32_t budget = 1000000U;
    while (IWDG->SR != 0U) {
        if (--budget == 0U) return 0;
    }
    Boot_WatchdogFeed();
    return 1;
}

void Boot_WatchdogFeed(void)
{
    IWDG->KR = 0xAAAAU;
}
