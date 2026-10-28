#include "app_health.h"
#include <string.h>

void AppHealth_Init(AppHealth *health, uint32_t now, uint32_t required_tasks)
{
    memset(health, 0, sizeof(*health));
    health->required_tasks = required_tasks;
    health->started_at = now;
    health->fault_latched = required_tasks == 0U ||
        (required_tasks & ~((1U << APP_HEALTH_MAX_TASKS) - 1U)) != 0U;
    for (unsigned i = 0; i < APP_HEALTH_MAX_TASKS; ++i)
        health->last_progress[i] = now;
}

void AppHealth_Progress(AppHealth *health, uint32_t tasks, uint32_t now)
{
    tasks &= health->required_tasks;
    for (unsigned i = 0; i < APP_HEALTH_MAX_TASKS; ++i) {
        if (tasks & (1U << i)) {
            /* A late completion must not conceal an already missed deadline. */
            if ((uint32_t)(now - health->last_progress[i]) > APP_HEALTH_PROGRESS_MS)
                health->fault_latched = 1U;
            health->last_progress[i] = now;
        }
    }
    health->seen_tasks |= tasks;
}

uint32_t AppHealth_Poll(AppHealth *health, uint32_t now)
{
    if (health->fault_latched) return APP_HEALTH_FAULT;
    for (unsigned i = 0; i < APP_HEALTH_MAX_TASKS; ++i) {
        if ((health->required_tasks & (1U << i)) &&
            (uint32_t)(now - health->last_progress[i]) > APP_HEALTH_PROGRESS_MS)
            health->fault_latched = 1U;
    }
    if (!health->confirmed &&
        (uint32_t)(now - health->started_at) >= APP_HEALTH_STARTUP_LIMIT_MS)
        health->fault_latched = 1U;
    if (health->fault_latched) return APP_HEALTH_FAULT;
    uint32_t action = APP_HEALTH_FEED;
    if (health->seen_tasks == health->required_tasks && !health->ready) {
        health->ready = 1U;
        health->ready_since = now;
    }
    if (!health->confirmed && health->ready &&
        (uint32_t)(now - health->ready_since) >= APP_HEALTH_STARTUP_STABLE_MS)
        action |= APP_HEALTH_CONFIRM;
    if (health->confirmed && !health->runtime_reported &&
        (uint32_t)(now - health->confirmed_at) >= APP_HEALTH_RUNTIME_STABLE_MS)
        action |= APP_HEALTH_RUNTIME_STABLE;
    return action;
}

void AppHealth_Confirmed(AppHealth *health, uint32_t now)
{
    health->confirmed = 1U;
    health->confirmed_at = now;
}

void AppHealth_RuntimeReported(AppHealth *health)
{
    health->runtime_reported = 1U;
}

void AppHealth_MaintenanceComplete(AppHealth *health, uint32_t since, uint32_t now)
{
    if (AppHealth_Poll(health, since) & APP_HEALTH_FAULT) return;
    uint32_t elapsed = now - since;
    for (unsigned i = 0; i < APP_HEALTH_MAX_TASKS; ++i)
        health->last_progress[i] += elapsed;
    health->started_at += elapsed;
    if (health->ready) health->ready_since += elapsed;
    if (health->confirmed) health->confirmed_at += elapsed;
}
