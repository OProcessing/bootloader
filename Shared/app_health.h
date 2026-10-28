#ifndef APP_HEALTH_H
#define APP_HEALTH_H

#include <stdint.h>

#define APP_HEALTH_MAX_TASKS        4U
#define APP_HEALTH_MAIN_TASK        (1U << 0)
#define APP_HEALTH_PROGRESS_MS      1000U
#define APP_HEALTH_STARTUP_STABLE_MS 2000U
#define APP_HEALTH_STARTUP_LIMIT_MS  8000U
#define APP_HEALTH_RUNTIME_STABLE_MS 30000U

enum {
    APP_HEALTH_FEED = 1U, APP_HEALTH_CONFIRM = 2U,
    APP_HEALTH_RUNTIME_STABLE = 4U, APP_HEALTH_FAULT = 8U
};

typedef struct {
    uint32_t required_tasks;
    uint32_t seen_tasks;
    uint32_t last_progress[APP_HEALTH_MAX_TASKS];
    uint32_t started_at;
    uint32_t ready_since;
    uint32_t confirmed_at;
    uint32_t ready;
    uint32_t confirmed;
    uint32_t runtime_reported;
    uint32_t fault_latched;
} AppHealth;

void AppHealth_Init(AppHealth *health, uint32_t now, uint32_t required_tasks);
/* Report real work completion, never simply from a periodic interrupt. */
void AppHealth_Progress(AppHealth *health, uint32_t tasks, uint32_t now);
uint32_t AppHealth_Poll(AppHealth *health, uint32_t now);
void AppHealth_Confirmed(AppHealth *health, uint32_t now);
void AppHealth_RuntimeReported(AppHealth *health);
/* Exclude a successful, known synchronous Flash operation from task deadlines.
 * IWDG remains running and is never refreshed inside that operation. */
void AppHealth_MaintenanceComplete(AppHealth *health, uint32_t since, uint32_t now);

#endif
