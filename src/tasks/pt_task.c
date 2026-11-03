/* PT board bench task: samples the transducer at 10 Hz and streams CSV over
 * the ST-LINK virtual COM port (USART3, 115200 8N1).
 *
 * At startup it prints '#' comment lines (pin mapping, calibration in use)
 * and a header, then one row per sample:
 *
 *   board_ms,data_counts,supply_counts,data_mv,supply_mv,ratio,pressure_psi,status
 *   12300,5931,59496,497,4993,0.0997,-0.20,ok
 *   12400,10,10,0,0,,,no_supply
 *   12500,,,,,,,adc_error
 *
 * board_ms is the board's clock: the FreeRTOS tick (1 kHz) when the read
 * started. tools/pt_capture.py records this stream on the host and adds the
 * computer's clock next to it, so the two can be checked against each other.
 *
 * Rows go out through the debug log. On the Sensor HAT these pins are PT2.
 */
#include "pt_task.h"

#include <stdio.h>

#include "tasks/log_task.h"
#include "drivers/pt_adc.h"
#include "app/pt_convert.h"

#define PT_PERIOD_MS  100U

/* VREF+ on the NUCLEO-H753ZI is VDDA = 3.3 V. Only the printed millivolts
 * depend on this; the ratio and pressure do not. */
#define PT_VREF_MV    3300U

/* NOTE set to the transducer on the board. 0.5-4.5 V on a 5 V supply is the
 * common ratiometric output; full scale is a placeholder. */
static const pt_calibration_t pt_calibration = {
    .ratio_at_zero = 0.10f,
    .ratio_at_full = 0.90f,
    .full_scale    = 500.0f,   /* psi */
};

/* newlib-nano's printf has no %f, so print value * 10^decimals as integer
 * parts. Handles the sign by hand so -0.05 doesn't print as 0.05. */
static int format_fixed(char *buffer, size_t buffer_size, float value, unsigned decimals)
{
    int32_t scale = 1;
    for (unsigned digit = 0; digit < decimals; digit++) {
        scale *= 10;
    }

    int32_t scaled = (int32_t)(value * (float)scale + (value < 0.0f ? -0.5f : 0.5f));
    const char *sign = scaled < 0 ? "-" : "";
    uint32_t magnitude = (uint32_t)(scaled < 0 ? -scaled : scaled);

    return snprintf(buffer, buffer_size, "%s%lu.%0*lu", sign,
                    (unsigned long)(magnitude / (uint32_t)scale),
                    (int)decimals, (unsigned long)(magnitude % (uint32_t)scale));
}

static void send_text(const char *text, int length)
{
    if (length > 0) {
        log_write(text, (size_t)length);
    }
}

/* Comment lines and the CSV header. Printed once, at startup. */
static void send_preamble(void)
{
    char line[128];
    int length;

    length = snprintf(line, sizeof line,
                      "# PT board: A0 = SENS_DATA_IN (PA3), A1 = SENS_PWR_IN (PC0)\r\n");
    send_text(line, length);

    length = snprintf(line, sizeof line, "# calibration: ratio_at_zero=");
    length += format_fixed(line + length, sizeof line - length, pt_calibration.ratio_at_zero, 4);
    length += snprintf(line + length, sizeof line - length, " ratio_at_full=");
    length += format_fixed(line + length, sizeof line - length, pt_calibration.ratio_at_full, 4);
    length += snprintf(line + length, sizeof line - length, " full_scale=");
    length += format_fixed(line + length, sizeof line - length, pt_calibration.full_scale, 2);
    length += snprintf(line + length, sizeof line - length, " psi\r\n");
    send_text(line, length);

    length = snprintf(line, sizeof line,
                      "board_ms,data_counts,supply_counts,data_mv,supply_mv,ratio,pressure_psi,status\r\n");
    send_text(line, length);
}

void vPtTask(void *pvParameters)
{
    (void)pvParameters;

    /* Here, not in main(): see pt_adc_init() in pt_adc.h. */
    pt_adc_init();

    send_preamble();

    TickType_t last_wake = xTaskGetTickCount();

    for (;;) {
        char line[128];
        int length;

        /* Ticks are milliseconds at configTICK_RATE_HZ = 1000. Wraps after
         * ~49 days. */
        unsigned long board_ms = (unsigned long)(xTaskGetTickCount() * portTICK_PERIOD_MS);

        pt_adc_sample_t sample;
        if (pt_adc_read(&sample) != HAL_OK) {
            length = snprintf(line, sizeof line, "%lu,,,,,,,adc_error\r\n", board_ms);
        } else {
            length = snprintf(line, sizeof line, "%lu,%u,%u,%lu,%lu,",
                              board_ms, sample.data_counts, sample.supply_counts,
                              (unsigned long)pt_counts_to_connector_mv(sample.data_counts, PT_VREF_MV),
                              (unsigned long)pt_counts_to_connector_mv(sample.supply_counts, PT_VREF_MV));

            float ratio;
            if (pt_ratio(sample.data_counts, sample.supply_counts, &ratio)) {
                length += format_fixed(line + length, sizeof line - length, ratio, 4);
                length += snprintf(line + length, sizeof line - length, ",");
                length += format_fixed(line + length, sizeof line - length,
                                       pt_pressure(&pt_calibration, ratio), 2);
                length += snprintf(line + length, sizeof line - length, ",ok\r\n");
            } else {
                length += snprintf(line + length, sizeof line - length, ",,no_supply\r\n");
            }
        }

        if (length > (int)sizeof line - 1) {
            length = (int)sizeof line - 1;
        }
        send_text(line, length);

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(PT_PERIOD_MS));
    }
}
