#ifndef APP_TASKS_H
#define APP_TASKS_H

#define APP_TASK_SENSOR  (1U << 0)
#define APP_TASK_CONTROL (1U << 1)
#define APP_TASK_COMMS   (1U << 2)
#define APP_TASK_REQUIRED (APP_TASK_SENSOR | APP_TASK_CONTROL | APP_TASK_COMMS)

/* Debugger injection values; cleared on each App reset. */
enum {
    APP_FAULT_NONE = 0,
    APP_FAULT_SENSOR_HANG = 1,
    APP_FAULT_SENSOR_REPORT = 2,
    APP_FAULT_CONTROL = 3,
    APP_FAULT_COMMS = 4,
    APP_FAULT_SUPERVISOR = 5
};

void AppTasks_Init(void);
void AppTasks_RunHealth(void *argument);

#endif
