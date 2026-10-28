#include "../Shared/app_health.h"
#include <assert.h>
#include <stdio.h>

static uint32_t progress(AppHealth *health, uint32_t now)
{
    AppHealth_Progress(health, APP_HEALTH_MAIN_TASK, now);
    return AppHealth_Poll(health, now);
}

static void test_normal(uint32_t start)
{
    AppHealth health;
    AppHealth_Init(&health, start, APP_HEALTH_MAIN_TASK);
    assert(progress(&health, start) == APP_HEALTH_FEED);
    for (uint32_t ms = 500; ms < 2000; ms += 500)
        assert(progress(&health, start + ms) == APP_HEALTH_FEED);
    uint32_t now = start + 2000U;
    assert(progress(&health, now) == (APP_HEALTH_FEED | APP_HEALTH_CONFIRM));
    AppHealth_Confirmed(&health, now);
    for (uint32_t ms = 500; ms < 30000; ms += 500)
        assert(progress(&health, now + ms) == APP_HEALTH_FEED);
    now += 30000;
    assert(progress(&health, now) == (APP_HEALTH_FEED | APP_HEALTH_RUNTIME_STABLE));
    AppHealth_RuntimeReported(&health);
    assert(progress(&health, now + 500U) == APP_HEALTH_FEED);
    assert(AppHealth_Poll(&health, now + 1501U) == APP_HEALTH_FAULT);
    /* Even restored progress cannot revive a latched fault. */
    assert(progress(&health, now + 1502U) == APP_HEALTH_FAULT);
}

int main(void)
{
    AppHealth health;
    test_normal(0U);
    test_normal(UINT32_MAX - 1000U); /* Tick wrap across startup + runtime. */
    AppHealth_Init(&health, 0, 0);
    assert(AppHealth_Poll(&health, 0) == APP_HEALTH_FAULT);
    AppHealth_Init(&health, 0, 1U << 7);
    assert(AppHealth_Poll(&health, 0) == APP_HEALTH_FAULT);
    AppHealth_Init(&health, 0, APP_HEALTH_MAIN_TASK);
    assert(AppHealth_Poll(&health, 1001) == APP_HEALTH_FAULT);
    AppHealth_Init(&health, 0, APP_HEALTH_MAIN_TASK);
    assert(progress(&health, 1001) == APP_HEALTH_FAULT); /* Late heartbeat before poll. */
    AppHealth_Init(&health, 0, 3U);
    assert(progress(&health, 0) == APP_HEALTH_FEED);
    assert(progress(&health, 1000) == APP_HEALTH_FEED);
    assert(progress(&health, 1001) == APP_HEALTH_FAULT); /* Missing second task. */
    AppHealth_Init(&health, 0, APP_HEALTH_MAIN_TASK);
    for (uint32_t now = 0; now < 8000; now += 500) {
        assert((progress(&health, now) & APP_HEALTH_FAULT) == 0U);
        /* Simulate a supervisor that ignores the request to confirm. */
    }
    assert(progress(&health, 8000) == APP_HEALTH_FAULT);
    /* Known blocking Flash operation is bounded by IWDG, not a task deadline. */
    AppHealth_Init(&health, 0, APP_HEALTH_MAIN_TASK);
    assert(progress(&health, 0) == APP_HEALTH_FEED);
    assert(progress(&health, 1000) == APP_HEALTH_FEED);
    AppHealth_MaintenanceComplete(&health, 1000, 6000);
    assert(progress(&health, 6000) == APP_HEALTH_FEED);
    assert(progress(&health, 6500) == APP_HEALTH_FEED);
    assert(progress(&health, 7000) == (APP_HEALTH_FEED | APP_HEALTH_CONFIRM));
    AppHealth_Confirmed(&health, 7000);
    AppHealth_MaintenanceComplete(&health, 7000, 12000);
    assert(health.confirmed_at == 12000);
    assert(progress(&health, 12000) == APP_HEALTH_FEED);
    /* Maintenance must not hide a deadline already missed before it started. */
    AppHealth_MaintenanceComplete(&health, 13001, 15000);
    assert(progress(&health, 15000) == APP_HEALTH_FAULT);
    puts("PASS: startup grace/deadline, task progress, latched failure, 30s runtime health,");
    puts("      multiple tasks, tick wrap and bounded Flash maintenance");
    return 0;
}
