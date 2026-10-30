#include "app_tasks.h"
#include "main.h"
#include "cmsis_os2.h"
#include "../../../Shared/app_health.h"
#include "../../../Shared/boot_state.h"
#include "../../../Shared/boot_watchdog.h"

typedef struct {
    uint32_t task;
    uint32_t completed_at;
} HealthReport;

typedef struct {
    uint32_t sequence;
    uint32_t value;
} SensorSample;

static osMessageQueueId_t health_queue;
static osMessageQueueId_t sample_queue;
osThreadId_t app_sensor_task;
osThreadId_t app_control_task;
osThreadId_t app_comms_task;

/* Health state and Flash writes are owned exclusively by defaultTask. */
AppHealth app_health;
volatile BootResult app_confirm_result = BOOT_BAD_STATE;
volatile BootResult app_runtime_result = BOOT_BAD_STATE;
volatile uint32_t app_health_action;
volatile uint32_t app_health_failed_tasks;
volatile uint32_t app_fault_injection;
volatile uint32_t app_report_drops;
volatile uint32_t app_sensor_heartbeat;
volatile uint32_t app_control_heartbeat;
volatile uint32_t app_comms_heartbeat;
volatile uint32_t app_sensor_value;
volatile uint32_t app_control_value;
volatile uint32_t app_telemetry_checksum;

static const osThreadAttr_t sensor_attributes = {
    .name = "sensorTask", .stack_size = 1024U, .priority = osPriorityNormal
};
static const osThreadAttr_t control_attributes = {
    .name = "controlTask", .stack_size = 1024U, .priority = osPriorityNormal
};
static const osThreadAttr_t comms_attributes = {
    .name = "commsTask", .stack_size = 1024U, .priority = osPriorityNormal
};

static uint32_t ticks_from_ms(uint32_t ms)
{
    return (ms * osKernelGetTickFreq() + 999U) / 1000U;
}

static void report_progress(uint32_t task)
{
    HealthReport report = {task, HAL_GetTick()};
    /* Nonblocking. A lost report does not count as progress. */
    if (osMessageQueuePut(health_queue, &report, 0U, 0U) != osOK)
        __atomic_fetch_add(&app_report_drops, 1U, __ATOMIC_RELAXED);
}

static void periodic_delay(uint32_t *wake, uint32_t period_ms)
{
    uint32_t period = ticks_from_ms(period_ms);
    *wake += period;
    uint32_t now = osKernelGetTickCount();
    /* Do not run a burst of catch-up work after a synchronous Flash write. */
    if ((int32_t)(*wake - now) <= 0) *wake = now + period;
    if (osDelayUntil(*wake) != osOK) Error_Handler();
}

static void sensor_thread(void *argument)
{
    (void)argument;
    uint32_t sequence = 0U;
    uint32_t wake = osKernelGetTickCount();
    for (;;) {
        if (app_fault_injection == APP_FAULT_SENSOR_HANG)
            for (;;) { __NOP(); }
        /* Synthetic sample, no physical ADC dependency. */
        ++sequence;
        SensorSample sample = {sequence, (sequence * 37U) & 0xFFFU};
        if (osMessageQueuePut(sample_queue, &sample, 0U, 0U) == osOK) {
            app_sensor_value = sample.value;
            ++app_sensor_heartbeat;
            if (app_fault_injection != APP_FAULT_SENSOR_REPORT)
                report_progress(APP_TASK_SENSOR);
        }
        periodic_delay(&wake, 100U);
    }
}

static void control_thread(void *argument)
{
    (void)argument;
    for (;;) {
        if (app_fault_injection == APP_FAULT_CONTROL) {
            osDelay(ticks_from_ms(50U));
            continue;
        }
        SensorSample sample;
        if (osMessageQueueGet(sample_queue, &sample, NULL, ticks_from_ms(200U)) == osOK) {
            app_control_value = (sample.value * 3U) / 4U;
            ++app_control_heartbeat;
            report_progress(APP_TASK_CONTROL);
        }
    }
}

static void comms_thread(void *argument)
{
    (void)argument;
    uint32_t wake = osKernelGetTickCount();
    for (;;) {
        if (app_fault_injection != APP_FAULT_COMMS) {
            /* Mock telemetry generation; no UART transmission in this example. */
            app_telemetry_checksum = app_control_value ^ app_comms_heartbeat;
            ++app_comms_heartbeat;
            report_progress(APP_TASK_COMMS);
        }
        periodic_delay(&wake, 250U);
    }
}

void AppTasks_Init(void)
{
    health_queue = osMessageQueueNew(16U, sizeof(HealthReport), NULL);
    sample_queue = osMessageQueueNew(8U, sizeof(SensorSample), NULL);
    if (health_queue == NULL || sample_queue == NULL) Error_Handler();
    app_sensor_task = osThreadNew(sensor_thread, NULL, &sensor_attributes);
    app_control_task = osThreadNew(control_thread, NULL, &control_attributes);
    app_comms_task = osThreadNew(comms_thread, NULL, &comms_attributes);
    if (app_sensor_task == NULL || app_control_task == NULL || app_comms_task == NULL)
        Error_Handler();
}

/* Flash stalls all tasks on this single-bank device. Suspend task switching
 * during synchronous storage but keep interrupts/HAL TIM6 running. The report
 * queue is drained before locking; workers cannot enqueue old timestamps while
 * maintenance adjusts the supervisor's deadlines. No Flash API runs in an ISR. */
static void store_health_result(int confirm)
{
    int32_t previous = osKernelLock();
    if (previous != 0) Error_Handler();
    uint32_t started = HAL_GetTick();
    BootResult result = confirm ? Boot_Confirm() : Boot_RuntimeHealthy();
    if (confirm) app_confirm_result = result;
    else app_runtime_result = result;
    if (result != BOOT_SUCCESS) Error_Handler();
    uint32_t completed = HAL_GetTick();
    AppHealth_MaintenanceComplete(&app_health, started, completed);
    if (confirm) AppHealth_Confirmed(&app_health, completed);
    else AppHealth_RuntimeReported(&app_health);
    if (osKernelRestoreLock(previous) < 0) Error_Handler();
}

void AppTasks_RunHealth(void *argument)
{
    (void)argument;
    AppHealth_Init(&app_health, HAL_GetTick(), APP_TASK_REQUIRED);
    for (;;) {
        if (app_fault_injection == APP_FAULT_SUPERVISOR)
            for (;;) osDelay(ticks_from_ms(20U));
        HealthReport report;
        while (osMessageQueueGet(health_queue, &report, NULL, 0U) == osOK)
            AppHealth_Progress(&app_health, report.task, report.completed_at);

        app_health_action = AppHealth_Poll(&app_health, HAL_GetTick());
        if ((app_health_action & APP_HEALTH_FAULT) == 0U) {
            if (app_health_action & APP_HEALTH_CONFIRM) store_health_result(1);
            if (app_health_action & APP_HEALTH_RUNTIME_STABLE) store_health_result(0);
            /* Maintenance may have latched a fault: check current state too. */
            if (!app_health.fault_latched && (app_health_action & APP_HEALTH_FEED))
                Boot_WatchdogFeed();
        }
        app_health_failed_tasks = app_health.failed_tasks;
        osDelay(ticks_from_ms(20U));
    }
}
