#ifndef BOOT_WATCHDOG_H
#define BOOT_WATCHDOG_H

/* Software-start IWDG, /256 and reload 4095: 32.768 s at nominal 32 kHz LSI.
 * Actual timeout varies with LSI frequency. No automatic IRQ-based refresh. */
int Boot_WatchdogStart(void);
void Boot_WatchdogFeed(void);

#endif
