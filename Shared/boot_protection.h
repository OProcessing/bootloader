#ifndef BOOT_PROTECTION_H
#define BOOT_PROTECTION_H
typedef enum {
    BOOT_PROTECTION_OK = 0,
    BOOT_PROTECTION_POWER_CYCLE_REQUIRED,
    BOOT_PROTECTION_ERROR,
    BOOT_PROTECTION_PCROP_MODE
} BootProtectionResult;
/* Only WRP sector 0/1 is changed. RDP, BOR, watchdog mode remain unchanged. */
BootProtectionResult Boot_ProtectBootloader(void);
#endif
