#include "tasks/radio_task.h"

#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "sx126x.h"

#include "main.h"
#include "app/command.h"
#include "app/radio_link.h"
#include "app/stand.h"
#include "board/board_pins.h"
#include "drivers/e22.h"
#include "drivers/pt_adc.h"
#include "tasks/actuator_task.h"
#include "tasks/datalog_task.h"
#include "tasks/interlock_task.h"
#include "tasks/lc_task.h"
#include "tasks/pt_task.h"
#include "tasks/remote_task.h"
#include "tasks/tc_task.h"

#define IDLE_MS          100U    // with no packets: how often to check the chip and the link
#define REPLY_DELAY_MS   5U      // lets the launch box turn round from sending to listening
#define TX_WAIT_MS       250U    // the chip itself gives up on a transmission after 200 ms
#define LINK_TIMEOUT_MS  60000U  // this long without the launch box: link lost, go safe.
                                 // The launch box counts the same time down: its PAD_HOLD_MS
#define SAFE_SEND_MS     50U     // the actuator task empties its queue every 10 ms

_Static_assert(PT_COUNT == 2 && TC_COUNT == 4 && LC_COUNT == 2,
               "radio_telemetry_t has room for 2 PTs, 4 TCs and 2 load cells");
// The sizes docs/radio-link.md gives the launch box.
_Static_assert(sizeof(radio_address_t) == 4 && sizeof(radio_tag_t) == 4 && sizeof(radio_state_t) == 6
               && sizeof(radio_telemetry_t) == 83, "packets must have no padding");

// What take_packet made of a packet.
typedef enum {
    PACKET_REJECTED,   // not this link's, or malformed
    PACKET_IGNORED,    // the other stand's telemetry
    PACKET_HEARD,      // from the launch box, to the other stand
    PACKET_ANSWER,     // from the launch box, to this stand
} packet_result_t;

// On air: the address, then the telemetry.
typedef struct __attribute__((packed)) {
    radio_address_t address;
    radio_telemetry_t telemetry;
} telemetry_packet_t;

static TaskHandle_t radio_task_handle;
static radio_status_t status;         // written by this task; copied in critical sections
static uint8_t last_sequence;         // of the last COMMAND or SAFE taken
// What STATE last asked of each output: 1 or 0, or a pulse. 0xFFFF: nothing yet.
static uint16_t state_sent[OUTPUT_COUNT];
static bool box_armed;                // the last STATE had ARMED set
static uint8_t next_name;             // the channel whose name goes in the next telemetry
static volatile bool send_requested;  // "radio send" at the console

// DIO1 rose: a packet has arrived or gone out.
void EXTI3_IRQHandler(void)
{
    __HAL_GPIO_EXTI_CLEAR_IT(RADIO_DIO1_Pin);
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(radio_task_handle, &woken);
    portYIELD_FROM_ISR(woken);
}

// Resets and configures the chip and starts listening.
static bool start(void)
{
    if (!e22_start() || !e22_receive()) {
        printf("radio: SX1262 not responding\r\n");
        return false;
    }
    // It answers, but may still have a complaint: bit 5, say, is a TCXO that
    // didn't start.
    sx126x_errors_mask_t errors;
    if (sx126x_get_device_errors(NULL, &errors) == SX126X_STATUS_OK && errors != 0) {
        printf("radio: SX1262 error flags 0x%04x\r\n", errors);
    }
    return true;
}

// Sends every output to its safe state, and drops the radio commands still
// waiting so none of them runs afterwards. The line goes to the data log too.
static void go_safe(const char *line)
{
    command_t command = { .type = COMMAND_SAFE };
    remote_flush();
    xQueueSend(actuator_queue, &command, pdMS_TO_TICKS(SAFE_SEND_MS));
    printf("%s\r\n", line);
    datalog_write(RECORD_TEXT, line, strlen(line));
}

// The newest reading of everything, in one packet.
static void fill_telemetry(radio_telemetry_t *telemetry)
{
    *telemetry = (radio_telemetry_t){
        .tag      = { RADIO_NETWORK_ID, RADIO_PROTOCOL_VERSION, RADIO_TELEMETRY },
        .flags    = status.aborted ? RADIO_FLAG_ABORTED : 0,
        .time_ms  = pdTICKS_TO_MS(xTaskGetTickCount()),
        .rssi_dbm = status.rssi_dbm,
        .snr_db   = status.snr_db,
    };

    for (unsigned i = 0; i < PT_COUNT; i++) {
        pt_reading_t pt;
        pt_latest(i, &pt);
        if (pt.powered) {
            telemetry->pt_psi[i] = pt.psi;
            telemetry->valid |= (uint8_t)(RADIO_VALID_PT1 << i);
        }
    }

    for (unsigned i = 0; i < LC_COUNT; i++) {
        lc_reading_t lc;
        lc_latest(i, &lc);
        if (lc.present) {
            telemetry->lc_code[i] = lc.code;
            telemetry->valid |= i == 0 ? RADIO_VALID_LC1 : RADIO_VALID_LC2;
        }
        if (lc.present && lc.calibrated) {
            telemetry->lc_newtons[i] = lc.newtons;
            telemetry->valid |= i == 0 ? RADIO_VALID_LC1_CALIBRATED : RADIO_VALID_LC2_CALIBRATED;
        }
    }

    // A thermocouple with a fault bit set (open circuit, say) reads nonsense.
    for (unsigned i = 0; i < TC_COUNT; i++) {
        tc_reading_t tc;
        tc_latest(i, &tc);
        if (tc.present && tc.value.faults == 0) {
            telemetry->tc_c[i] = tc.value.temp_c;
            telemetry->valid |= (uint8_t)(RADIO_VALID_TC1 << i);
        }
    }

    record_actuators_t actuators;
    actuator_latest(&actuators);
    telemetry->solenoids = actuators.solenoids;
    telemetry->vbat_mv = (uint16_t)actuators.vbat_mv;
    for (unsigned i = 0; i < SERVO_COUNT; i++) {
        telemetry->servo_us[i] = actuators.servo_us[i];
    }

    // The valves this stand drives, numbered by their switch on the launch box,
    // and which of them are open.
    for (unsigned output = 0; output < OUTPUT_COUNT; output++) {
        const output_t *row = stand_output(output);
        int valve = stand_valve(row);
        if (valve < 0) {
            continue;
        }
        uint16_t drive = output < SOLENOID_COUNT ? (actuators.solenoids >> output) & 1U
                                                 : actuators.servo_us[output - SOLENOID_COUNT];
        telemetry->valves_mapped |= (uint8_t)(1U << valve);
        if (stand_is_open(output, row, drive)) {
            telemetry->valves |= (uint8_t)(1U << valve);
        }
    }

    datalog_status_t log;
    datalog_status(&log);
    if (log.recording) {
        telemetry->flags |= RADIO_FLAG_LOGGING;
        telemetry->log_bytes = log.bytes_written;
    }

    // One name per packet, each channel in turn.
    telemetry->name_channel = next_name;
    strncpy(telemetry->name, stand_channel_name(next_name), RADIO_NAME_SIZE - 1);
    next_name = (uint8_t)((next_name + 1U) % CHANNEL_COUNT);
}


// Transmits one telemetry packet, waits until it's out, and listens again.
static void send_telemetry(void)
{
    // To everyone, so a radio that only listens takes it as well as the launch box.
    telemetry_packet_t packet = {
        .address = { .to = RADIO_NODE_ALL, .from = stand_number(), .sequence = last_sequence },
    };
    fill_telemetry(&packet.telemetry);

    if (e22_send((const uint8_t *)&packet, sizeof packet)) {
        // Forget any DIO1 edge from before the transmission started. The next
        // one is this packet going out, about 30 ms from now.
        ulTaskNotifyTake(pdTRUE, 0);
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(TX_WAIT_MS));

        sx126x_irq_mask_t events;
        if (sx126x_get_and_clear_irq_status(NULL, &events) == SX126X_STATUS_OK && (events & SX126X_IRQ_TX_DONE)) {
            status.sent++;
        }
    }
    e22_receive();
}

// Makes the outputs that drive valves follow a STATE packet: the positions of
// the launch box's switches.
// An output gets a command only when what the launch box wants of it changes:
// state_sent remembers what it last asked for, and nothing but a new position
// from the launch box changes that. So a packet that was lost is made up by the
// next one, a command typed at the stand's console always takes effect and
// stays until the operator moves that valve's switch, and after a "safe"
// nothing goes back by itself.
static void apply_state(const radio_state_t *state)
{
    bool armed = (state->flags & RADIO_STATE_ARMED) != 0;
    if (!armed) {
        // Key off, or the launch box's own abort: it has seen that this stand
        // went safe by itself, so the stand may follow it again.
        status.aborted = false;
        box_armed = false;
    } else if (!box_armed) {
        // The launch box has just armed: start the data log. The remote task
        // does it, as if "log start" had been sent, because opening the file
        // takes a few hundred ms and the radio has to keep answering.
        static const char line[] = "log start";
        datalog_status_t log;
        datalog_status(&log);
        // No room for the line: try again on the next STATE.
        box_armed = log.recording || remote_submit(line, sizeof line - 1);
    }
    bool follow_valves = (armed || (state->flags & RADIO_STATE_ABORT)) && !status.aborted;

    for (unsigned output = 0; output < OUTPUT_COUNT; output++) {
        const output_t *row = stand_output(output);
        int valve = stand_valve(row);

        bool open;
        if (valve >= 0) {
            if (!follow_valves) {
                continue;
            }
            open = (state->valves >> valve) & 1U;
        } else if (row->drives == IGNITER_1 || row->drives == IGNITER_2) {
            // An igniter is on only while armed and told to be, and goes off
            // with the key: unlike a valve it never holds.
            uint8_t flag = row->drives == IGNITER_1 ? RADIO_STATE_IGNITER_1 : RADIO_STATE_IGNITER_2;
            open = armed && !status.aborted && (state->flags & flag);
        } else {
            continue;
        }

        uint16_t drive = stand_drive(output, row, open);
        if (drive == state_sent[output]) {
            continue;
        }
        command_t command;
        if (output < SOLENOID_COUNT) {
            command = (command_t){ .type = COMMAND_SOLENOID, .channel = (uint8_t)output, .on = drive != 0 };
        } else {
            command = (command_t){ .type = COMMAND_SERVO, .channel = (uint8_t)(output - SOLENOID_COUNT),
                                   .pulse_us = drive };
        }
        // No room: not marked as sent, so the next STATE tries again.
        if (xQueueSend(actuator_queue, &command, 0) == pdPASS) {
            state_sent[output] = drive;
        }
    }
}

// Reads the packet that just arrived and acts on it.
static packet_result_t take_packet(void)
{
    uint8_t packet[sizeof(radio_address_t) + sizeof(radio_tag_t) + RADIO_COMMAND_MAX];
    uint8_t length;
    int8_t rssi_dbm;
    int8_t snr_db;
    radio_address_t address;
    radio_tag_t tag;

    if (!e22_read_packet(packet, sizeof packet, &length, &rssi_dbm, &snr_db)
        || length < sizeof address + sizeof tag) {
        return PACKET_REJECTED;
    }
    memcpy(&address, packet, sizeof address);
    memcpy(&tag, &packet[sizeof address], sizeof tag);
    if (tag.network != RADIO_NETWORK_ID || tag.version != RADIO_PROTOCOL_VERSION) {
        return PACKET_REJECTED;
    }
    if (address.from != RADIO_NODE_BOX) {
        return address.from <= RADIO_STAND_MAX && tag.type == RADIO_TELEMETRY ? PACKET_IGNORED
                                                                              : PACKET_REJECTED;
    }

    bool mine = address.to == stand_number();
    const uint8_t *text = &packet[sizeof address + sizeof tag];
    size_t text_length = length - sizeof address - sizeof tag;

    switch (tag.type) {
    case RADIO_POLL:
        break;

    case RADIO_STATE: {
        // Acted on whoever it's addressed to: both stands follow every one.
        radio_state_t state;
        if (length != sizeof address + sizeof state) {
            return PACKET_REJECTED;
        }
        memcpy(&state, &packet[sizeof address], sizeof state);
        apply_state(&state);
        break;
    }

    case RADIO_SAFE:
        // Acted on every time it arrives, repeated or not.
        if (mine || address.to == RADIO_NODE_ALL) {
            go_safe("radio: SAFE from the launch box");
        }
        if (mine) {
            last_sequence = address.sequence;
        }
        break;

    case RADIO_COMMAND:
        // A repeat of the one just taken is only answered again. If the remote
        // task has no room the command isn't taken; the launch box sees no
        // acknowledgement and sends it again.
        if (mine && address.sequence != last_sequence && remote_submit((const char *)text, text_length)) {
            last_sequence = address.sequence;
        }
        break;

    default:
        return PACKET_REJECTED;
    }

    status.received++;
    status.rssi_dbm = rssi_dbm;
    status.snr_db = snr_db;
    return mine ? PACKET_ANSWER : PACKET_HEARD;
}

void vRadioTask(void *pvParameters)
{
    (void)pvParameters;

    radio_task_handle = xTaskGetCurrentTaskHandle();
    bool hat_powered = false;
    uint32_t last_packet_ms = 0;

    for (;;) {
        // Woken by DIO1. The timeout is for everything that isn't a packet: the
        // HAT's power, the chip's health, the link timer and "radio send".
        bool dio1 = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(IDLE_MS)) > 0;
        uint32_t now_ms = pdTICKS_TO_MS(xTaskGetTickCount());

        // The chip loses its setup without power, so start it each time. A link
        // that ends this way needs no "safe": the interlock has already switched
        // everything off.
        bool powered_now = interlock_current_state() == INTERLOCK_ENABLED;
        if (powered_now != hat_powered) {
            hat_powered = powered_now;
            status.linked = false;
            status.aborted = false;
            // The stand's number is in the HAT's flash, and has to be right
            // before the first answer. Every output is off after a power-up, so
            // the first STATE that's followed sets them all.
            if (hat_powered) {
                bool loaded = stand_load();
                printf("stand %u, %s\r\n", stand_number(), loaded ? "from flash" : "none saved in flash");
                memset(state_sent, 0xFF, sizeof state_sent);
                box_armed = false;   // the log stopped with the HAT's power: start it again
            }
            status.started = hat_powered && start();
            continue;
        }
        if (!status.started) {
            continue;
        }

        if (dio1) {
            // What DIO1 rose for. Reading it clears it, so DIO1 can rise again.
            sx126x_irq_mask_t events = SX126X_IRQ_NONE;
            sx126x_get_and_clear_irq_status(NULL, &events);
            packet_result_t result = PACKET_IGNORED;
            if (events & (SX126X_IRQ_HEADER_ERROR | SX126X_IRQ_CRC_ERROR)) {
                result = PACKET_REJECTED;
            } else if (events & SX126X_IRQ_RX_DONE) {
                result = take_packet();
            }

            if (result == PACKET_REJECTED) {
                status.rejected++;
            }
            // Any packet from the launch box keeps the link up, whichever stand
            // it's for.
            if (result == PACKET_HEARD || result == PACKET_ANSWER) {
                last_packet_ms = now_ms;
                if (!status.linked) {
                    status.linked = true;
                    printf("radio: link up\r\n");
                }
            }
            // One addressed to this stand gets one answer.
            if (result == PACKET_ANSWER) {
                vTaskDelay(pdMS_TO_TICKS(REPLY_DELAY_MS));
                send_telemetry();
            }
        } else if (!e22_listening()) {
            // Quiet, and not in receive mode: the chip has reset itself, on a
            // supply dip as the amplifier keyed up, say. Set it up again.
            status.restarts++;
            printf("radio: SX1262 left receive mode, restarting it\r\n");
            status.started = start();
            continue;
        }

        if (send_requested) {
            send_requested = false;
            send_telemetry();
        }

        if (status.linked && now_ms - last_packet_ms >= LINK_TIMEOUT_MS) {
            status.linked = false;
            status.aborted = true;
            go_safe("radio: link lost, going safe");
        }
    }
}

void radio_status(radio_status_t *copy)
{
    taskENTER_CRITICAL();
    *copy = status;
    taskEXIT_CRITICAL();
}

bool radio_send_test(void)
{
    if (!status.started) {
        return false;
    }
    send_requested = true;
    return true;
}
