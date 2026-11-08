#include "tasks/pt_task.h"

#include "FreeRTOS.h"
#include "task.h"

#include "main.h"
#include "app/pt_convert.h"
#include "drivers/pt_adc.h"

// VREF+ is VDDA, 3.3 V on the Nucleo. Only the millivolts depend on it; the
// ratio and pressure don't.
#define VREF_MV  3300U

// 0.5-4.5 V of a 5 V supply. On hand: one 1600, two 1000 and two 500 psi
// transducers. Set full_scale to whichever ends up on each port.
static const pt_calibration_t calibrations[PT_COUNT] = {
    { .ratio_at_zero = 0.10f, .ratio_at_full = 0.90f, .full_scale = 1000.0f },   // PT1 ox tank
    { .ratio_at_zero = 0.10f, .ratio_at_full = 0.90f, .full_scale = 500.0f },    // PT2 chamber
};

static TaskHandle_t pt_task_handle;
static pt_reading_t latest[PT_COUNT];

// Tells the task which half of the buffer is ready. If it's still busy with the
// last one, the newer half replaces it.
static void block_ready_from_isr(uint32_t half)
{
    BaseType_t woken = pdFALSE;
    xTaskNotifyFromISR(pt_task_handle, half, eSetValueWithOverwrite, &woken);
    portYIELD_FROM_ISR(woken);
}

// The HAL calls these from the DMA interrupt. Only ADC1 uses DMA.
void HAL_ADC_ConvHalfCpltCallback(ADC_HandleTypeDef *hadc)
{
    (void)hadc;
    block_ready_from_isr(0);
}

void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
    (void)hadc;
    block_ready_from_isr(1);
}

void vPtTask(void *pvParameters)
{
    (void)pvParameters;

    // The callbacks need the handle, so it's set before the first one can run.
    pt_task_handle = xTaskGetCurrentTaskHandle();
    pt_adc_start();

    for (;;) {
        uint32_t half;
        xTaskNotifyWait(0, 0, &half, portMAX_DELAY);
        const pt_adc_sample_t *block = pt_adc_block(half);

        for (unsigned i = 0; i < PT_COUNT; i++) {
            // Average the 10 samples. Supply and output were taken together, so
            // their ratio holds even if the supply moved during the block.
            uint32_t supply_sum = 0;
            uint32_t data_sum = 0;
            for (unsigned n = 0; n < PT_BLOCK_SAMPLES; n++) {
                supply_sum += block[n].pt[i].supply_counts;
                data_sum += block[n].pt[i].data_counts;
            }
            uint16_t supply_counts = (uint16_t)(supply_sum / PT_BLOCK_SAMPLES);
            uint16_t data_counts = (uint16_t)(data_sum / PT_BLOCK_SAMPLES);

            pt_reading_t reading = {
                .supply_mv = pt_counts_to_connector_mv(supply_counts, VREF_MV),
                .data_mv   = pt_counts_to_connector_mv(data_counts, VREF_MV),
            };
            reading.powered = pt_ratio(data_counts, supply_counts, &reading.ratio); //make sure the PT's are actually powered
            if (reading.powered) {
                reading.psi = pt_pressure(&calibrations[i], reading.ratio);
            }

            // A reading is several words, so copy it whole with nothing in between.
            taskENTER_CRITICAL();
            latest[i] = reading;
            taskEXIT_CRITICAL();
        }
    }
}

void pt_latest(unsigned pt, pt_reading_t *reading)
{
    taskENTER_CRITICAL();
    *reading = latest[pt];
    taskEXIT_CRITICAL();
}
