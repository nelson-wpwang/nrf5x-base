/* Copyright (c) 2014 Nordic Semiconductor. All Rights Reserved.
 *
 * The information contained herein is property of Nordic Semiconductor ASA.
 * Terms and conditions of usage are described in detail in NORDIC
 * SEMICONDUCTOR STANDARD SOFTWARE LICENSE AGREEMENT.
 *
 * Licensees are granted free, non-transferable use of the information. NO
 * WARRANTY of ANY KIND is provided. This heading must NOT be removed from
 * the file.
 *
 */
#include "sdk_common.h"
#if NRF_MODULE_ENABLED(APP_TIMER)
#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"

#include "app_timer.h"
#include <stdlib.h>
#include <string.h>
#include "nrf.h"
#include "app_error.h"

/* Check if RTC FreeRTOS version is used */
#if configTICK_SOURCE != FREERTOS_USE_RTC
#error app_timer in FreeRTOS variant have to be used with RTC tick source configuration. Default configuration have to be used in other case.
#endif

/**
 * @brief Waiting time for the timer queue
 *
 * Number of system ticks to wait for the timer queue to put the message.
 * It is strongly recommended to set this to the value bigger than 1.
 * In other case if timer message queue is full - any operation on timer may fail.
 * @note
 * Timer functions called from interrupt context would never wait.
 */
#define APP_TIMER_WAIT_FOR_QUEUE 2

/**@brief This structure keeps information about osTimer.*/
typedef struct
{
    void                      * argument;
    TimerHandle_t               osHandle;
    app_timer_timeout_handler_t func;
    /**
     * This member is to make sure that timer function is only called if timer is running.
     * FreeRTOS may have timer running even after stop function is called,
     * because it processes commands in Timer task and stopping function only puts command into the queue. */
    bool                        active;
}app_timer_info_t;

/**
 * @brief Prescaler that was set by the user
 *
 * In FreeRTOS version of app_timer the prescaler setting is constant and done by the operating system.
 * But the application expect the prescaler to be set according to value given in setup and then
 * calculate required ticks using this value.
 * For compatibility we remember the value set and use it for recalculation of required timer setting.
 */
static uint32_t m_prescaler;

/* Check if freeRTOS timers are activated */
#if configUSE_TIMERS == 0
    #error app_timer for freeRTOS requires configUSE_TIMERS option to be activated.
#endif

/* Check if app_timer_t variable type can held our app_timer_info_t structure */
STATIC_ASSERT(sizeof(app_timer_info_t) <= sizeof(app_timer_t));


/**
 * @brief Internal callback function for the system timer
 *
 * Internal function that is called from the system timer.
 * It gets our parameter from timer data and sends it to user function.
 * @param[in] xTimer Timer handler
 */
static void app_timer_callback(TimerHandle_t xTimer)
{
    app_timer_info_t * pinfo = (app_timer_info_t*)(pvTimerGetTimerID(xTimer));
    ASSERT(pinfo->osHandle == xTimer);
    ASSERT(pinfo->func != NULL);

    if (pinfo->active)
        pinfo->func(pinfo->argument);
}


uint32_t app_timer_init(uint32_t                      prescaler,
                        uint8_t                       op_queues_size,
                        void                        * p_buffer,
                        app_timer_evt_schedule_func_t evt_schedule_func)
{
    UNUSED_PARAMETER(op_queues_size);
    UNUSED_PARAMETER(p_buffer);
    UNUSED_PARAMETER(evt_schedule_func);

    m_prescaler = prescaler + 1;

    return NRF_SUCCESS;
}


uint32_t app_timer_create(app_timer_id_t const *      p_timer_id,
                          app_timer_mode_t            mode,
                          app_timer_timeout_handler_t timeout_handler)
{
    app_timer_info_t * pinfo = (app_timer_info_t*)(*p_timer_id);
    uint32_t      err_code = NRF_SUCCESS;
    unsigned long timer_mode;

    if ((timeout_handler == NULL) || (p_timer_id == NULL))
    {
        return NRF_ERROR_INVALID_PARAM;
    }
    if (pinfo->active)
    {
        return NRF_ERROR_INVALID_STATE;
    }

    if (pinfo->osHandle == NULL)
    {
        /* New timer is created */
        memset(pinfo, 0, sizeof(app_timer_info_t));

        if (mode == APP_TIMER_MODE_SINGLE_SHOT)
            timer_mode = pdFALSE;
        else
            timer_mode = pdTRUE;

        pinfo->func = timeout_handler;
        pinfo->osHandle = xTimerCreate(" ", 1000, timer_mode, pinfo, app_timer_callback);

        if (pinfo->osHandle == NULL)
            err_code = NRF_ERROR_NULL;
    }
    else
    {
        /* Timer cannot be reinitialized using FreeRTOS API */
        return NRF_ERROR_INVALID_STATE;
    }

    return err_code;
}


uint32_t app_timer_start(app_timer_id_t timer_id, uint32_t timeout_ticks, void * p_context)
{
    app_timer_info_t * pinfo = (app_timer_info_t*)(timer_id);
    TimerHandle_t hTimer = pinfo->osHandle;
    uint32_t rtc_prescaler = portNRF_RTC_REG->PRESCALER  + 1;
    /* Get back the microseconds to wait */
    uint32_t timeout_corrected = ROUNDED_DIV(timeout_ticks * m_prescaler, rtc_prescaler);

    if (hTimer == NULL)
    {
        return NRF_ERROR_INVALID_STATE;
    }
    if (pinfo->active && (xTimerIsTimerActive(hTimer) != pdFALSE))
    {
        // Timer already running - exit silently
        return NRF_SUCCESS;
    }

    pinfo->argument = p_context;

    if (__get_IPSR() != 0)
    {
        BaseType_t yieldReq = pdFALSE;
        if (xTimerChangePeriodFromISR(hTimer, timeout_corrected, &yieldReq) != pdPASS)
        {
            return NRF_ERROR_NO_MEM;
        }

        if ( xTimerStartFromISR(hTimer, &yieldReq) != pdPASS )
        {
            return NRF_ERROR_NO_MEM;
        }

        portYIELD_FROM_ISR(yieldReq);
    }
    else
    {
        if (xTimerChangePeriod(hTimer, timeout_corrected, APP_TIMER_WAIT_FOR_QUEUE) != pdPASS)
        {
            return NRF_ERROR_NO_MEM;
        }

        if (xTimerStart(hTimer, APP_TIMER_WAIT_FOR_QUEUE) != pdPASS)
        {
            return NRF_ERROR_NO_MEM;
        }
    }

    pinfo->active = true;
    return NRF_SUCCESS;
}


uint32_t app_timer_stop(app_timer_id_t timer_id)
{
    app_timer_info_t * pinfo = (app_timer_info_t*)(timer_id);
    TimerHandle_t hTimer = pinfo->osHandle;
    if (hTimer == NULL)
    {
        return NRF_ERROR_INVALID_STATE;
    }

    if (__get_IPSR() != 0)
    {
        BaseType_t yieldReq = pdFALSE;
        if (xTimerStopFromISR(timer_id, &yieldReq) != pdPASS)
        {
            return NRF_ERROR_NO_MEM;
        }
        portYIELD_FROM_ISR(yieldReq);
    }
    else
    {
        if (xTimerStop(timer_id, APP_TIMER_WAIT_FOR_QUEUE) != pdPASS)
        {
            return NRF_ERROR_NO_MEM;
        }
    }

    pinfo->active = false;
    return NRF_SUCCESS;
}
#endif //NRF_MODULE_ENABLED(APP_TIMER)
