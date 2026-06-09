/*
 * Skate Controller for Subway Surfers
 * RTOS firmware: FreeRTOS + Edge Impulse (ADXL345 + Bluetooth)
 *
 * Hardware (Pico 2 / RP2350):
 *   ADXL345 IMU  : I2C1 SDA=GP18, SCL=GP19
 *   Bluetooth HC : UART0 TX=GP12, RX=GP13, 9600 baud
 *   Buttons      : GP4 (Start), GP5 (Pause), GP6 (Volume)
 *   LED RGB      : GP7 (R), GP8 (G), GP9 (B)  -- active HIGH
 *   Debug GPIO   : GP20 (sensor task), GP21 (BT task), GP22 (LED task)
 *
 * Model classes: 0=idle, 1=updown (jump), 2=wave (dodge)
 * BT commands sent: 'J'=jump, 'W'=wave, 'I'=idle, 'S'=start, 'P'=pause, 'V'=volume
 */

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "hardware/uart.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

#include "mpu6050.h"
#include "pins.h"

/* ── MPU6050 driver (I2C0, SDA=GP0, SCL=GP1) ────────────────────────────── */
#define MPU_I2C      i2c0
#define MPU_SDA_PIN  16
#define MPU_SCL_PIN  17
#define MPU_ADDR     MPU6050_I2C_DEFAULT   /* 0x68 */
#define MPU_ACCEL_2G_SENSITIVITY 16384.0f  /* LSB/g at ±2g range */

static void mpu6050_write_reg(uint8_t reg, uint8_t val) {
    uint8_t buf[2] = {reg, val};
    i2c_write_blocking(MPU_I2C, MPU_ADDR, buf, 2, false);
}

static bool mpu6050_init(void) {
    i2c_init(MPU_I2C, 400 * 1000);
    gpio_set_function(MPU_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(MPU_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(MPU_SDA_PIN);
    gpio_pull_up(MPU_SCL_PIN);

    /* Wake up (exits sleep mode) */
    mpu6050_write_reg(MPUREG_PWR_MGMT_1, 0x00);
    sleep_ms(100);

    /* Verify device is present: WHO_AM_I should return 0x68 */
    uint8_t reg = MPUREG_WHOAMI;
    uint8_t id  = 0;
    i2c_write_blocking(MPU_I2C, MPU_ADDR, &reg, 1, true);
    i2c_read_blocking (MPU_I2C, MPU_ADDR, &id,  1, false);
    if (id != 0x68) return false;

    /* Accel range ±2g, gyro range ±250°/s (defaults, no change needed) */
    mpu6050_write_reg(MPUREG_ACCEL_CONFIG, 0x00);

    /* Sample rate: 1kHz / (1 + SMPLRT_DIV) — set 100 Hz (divider=9) */
    mpu6050_write_reg(MPUREG_SMPLRT_DIV, 9);

    return true;
}

static void mpu6050_read_accel(float *ax, float *ay, float *az) {
    uint8_t reg = MPUREG_ACCEL_XOUT_H;
    uint8_t raw[6];
    i2c_write_blocking(MPU_I2C, MPU_ADDR, &reg, 1, true);
    i2c_read_blocking (MPU_I2C, MPU_ADDR, raw, 6, false);

    int16_t x = (int16_t)((raw[0] << 8) | raw[1]);
    int16_t y = (int16_t)((raw[2] << 8) | raw[3]);
    int16_t z = (int16_t)((raw[4] << 8) | raw[5]);

    *ax = (x / MPU_ACCEL_2G_SENSITIVITY) * 9.80665f;
    *ay = (y / MPU_ACCEL_2G_SENSITIVITY) * 9.80665f;
    *az = (z / MPU_ACCEL_2G_SENSITIVITY) * 9.80665f;
}

/* EI porting layer — plain C++ linkage (matches tflite compiled model declarations) */
#include "edge-impulse-sdk/dsp/returntypes.h"

void ei_printf(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
}

void ei_printf_float(float f) { printf("%f", f); }
void ei_putchar(char c)       { putchar(c); }
char ei_getchar(void)         { return (char)getchar_timeout_us(0); }

/* Thread-safe memory: use FreeRTOS heap when scheduler is running */
void *ei_malloc(size_t size) {
    return (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED)
        ? pvPortMalloc(size)
        : malloc(size);
}
void *ei_calloc(size_t count, size_t size) {
    void *ptr = ei_malloc(count * size);
    if (ptr) memset(ptr, 0, count * size);
    return ptr;
}
void ei_free(void *ptr) {
    if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED)
        vPortFree(ptr);
    else
        free(ptr);
}

EI_IMPULSE_ERROR ei_run_impulse_check_canceled(void) { return EI_IMPULSE_OK; }
void ei_serial_set_baudrate(int baud)                { (void)baud; }
uint64_t ei_read_timer_ms(void)                      { return to_ms_since_boot(get_absolute_time()); }
uint64_t ei_read_timer_us(void)                      { return to_us_since_boot(get_absolute_time()); }
EI_IMPULSE_ERROR ei_sleep(int32_t ms)                { vTaskDelay(pdMS_TO_TICKS(ms)); return EI_IMPULSE_OK; }

#include "edge-impulse-sdk/classifier/ei_run_classifier.h"
#include "edge-impulse-sdk/dsp/numpy.hpp"

/* -------------------------------------------------------------------------- */
/* Pin / peripheral config                                                     */
/* -------------------------------------------------------------------------- */
#define BT_UART_ID   uart1
#define BT_TX_PIN    5
#define BT_RX_PIN    4
#define BT_BAUD      9600

/* Debug GPIO for WCET / jitter measurement with an oscilloscope */
#define DBG_SENSOR   20
#define DBG_BT       21
#define DBG_LED      22

/* Button IDs sent over xQueueButtons */
#define BTN_ID_START    0   /* GP18 BRANCO  */
#define BTN_ID_PAUSE    1   /* GP19 AZUL    */
#define BTN_ID_VOL_UP   2   /* GP20 AMARELO */
#define BTN_ID_VOL_DOWN 3   /* GP21 VERDE   */

/*
 * Motion label indices — devem bater com a ordem ALFABÉTICA das classes
 * no Edge Impulse (ele ordena por nome). Ao exportar o modelo, confirme em:
 *   ei-model/model-parameters/model_variables.h
 *   ei_classifier_inferencing_categories[] = { ..., ..., ... }
 *
 * Classes sugeridas para treino (nomes em inglês, ordem alfabética):
 *   0 = "backward"  → tail down / empurrar para trás  → agachar no jogo
 *   1 = "forward"   → nose down / empurrar para frente → pular no jogo
 *   2 = "idle"      → parado / rolando reto            → nada
 *   3 = "left"      → inclinar para esquerda           → mover esquerda
 *   4 = "right"     → inclinar para direita            → mover direita
 */
#define LABEL_CROUNCH  0
#define LABEL_IDLE     1
#define LABEL_JUMP     2
#define LABEL_LEFT     3
#define LABEL_RIGHT    4

/* BT command bytes (devem bater com controller.py) */
#define CMD_IDLE     'I'
#define CMD_JUMP     'J'
#define CMD_LEFT     'L'
#define CMD_RIGHT    'R'
#define CMD_CROUCH   'C'
#define CMD_START    'S'
#define CMD_PAUSE    'P'
#define CMD_VOL_UP   'V'
#define CMD_VOL_DOWN 'D'

/* -------------------------------------------------------------------------- */
/* FreeRTOS objects                                                            */
/* -------------------------------------------------------------------------- */
static QueueHandle_t xQueueMotion;   /* sensor_ai  -> bluetooth (int label)  */
static QueueHandle_t xQueueButtons;  /* ISR        -> bluetooth (int btn id) */
static QueueHandle_t xQueueLed;      /* sensor_ai  -> led       (int label)  */

/* -------------------------------------------------------------------------- */
/* ISR: button handler                                                         */
/* -------------------------------------------------------------------------- */
static void gpio_irq_handler(uint gpio, uint32_t events) {
    if (!(events & GPIO_IRQ_EDGE_FALL))
        return;

    int btn_id = -1;
    if (gpio == BTN_PIN_R)        btn_id = BTN_ID_START;
    if (gpio == BTN_PIN_G)        btn_id = BTN_ID_PAUSE;
    if (gpio == BTN_PIN_B)        btn_id = BTN_ID_VOL_UP;
    if (gpio == BTN_PIN_VOL_DOWN) btn_id = BTN_ID_VOL_DOWN;

    if (btn_id < 0)
        return;

    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(xQueueButtons, &btn_id, &woken);
    portYIELD_FROM_ISR(woken);
}

/* -------------------------------------------------------------------------- */
/* task_sensor_ai                                                              */
/* Reads ADXL345 at ~83 Hz, fills EI input buffer, runs inference.            */
/* -------------------------------------------------------------------------- */
static void task_sensor_ai(void *param) {
    (void)param;

    if (!mpu6050_init()) {
        ei_printf("[sensor] MPU6050 init failed! Check SDA=GP0, SCL=GP1, addr=0x68\n");
        vTaskDelete(NULL);
        return;
    }
    ei_printf("[sensor] MPU6050 ready.\n");

    static float ei_buffer[EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE];
    int sample_idx = 0;

    TickType_t xLastWakeTime = xTaskGetTickCount();
    /* EI_CLASSIFIER_INTERVAL_MS = 12.048... ms => ~83 Hz */
    const TickType_t xPeriod = pdMS_TO_TICKS((uint32_t)EI_CLASSIFIER_INTERVAL_MS);

    while (1) {
        gpio_put(DBG_SENSOR, 1); /* --- WCET start --- */

        float x, y, z;
        mpu6050_read_accel(&x, &y, &z);
        ei_buffer[sample_idx * EI_CLASSIFIER_RAW_SAMPLES_PER_FRAME + 0] = x;
        ei_buffer[sample_idx * EI_CLASSIFIER_RAW_SAMPLES_PER_FRAME + 1] = y;
        ei_buffer[sample_idx * EI_CLASSIFIER_RAW_SAMPLES_PER_FRAME + 2] = z;
        sample_idx++;

        if (sample_idx >= EI_CLASSIFIER_RAW_SAMPLE_COUNT) {
            /* Full window collected – run inference (this tick has WCET spike) */
            signal_t signal;
            int err = numpy::signal_from_buffer(
                ei_buffer, EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE, &signal);

            if (err == 0) {
                ei_impulse_result_t result = {0};
                EI_IMPULSE_ERROR ei_err =
                    run_classifier(&signal, &result, false /* no debug */);

                if (ei_err == EI_IMPULSE_OK) {
                    /* Find label with highest confidence */
                    int best = LABEL_IDLE;
                    for (int i = 1; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
                        if (result.classification[i].value >
                            result.classification[best].value)
                            best = i;
                    }

                    /* Only act if confidence clears the model threshold */
                    if (result.classification[best].value >= EI_CLASSIFIER_THRESHOLD) {
                        xQueueSend(xQueueMotion, &best, 0);
                        xQueueSend(xQueueLed,    &best, 0);
                    } else {
                        int idle = LABEL_IDLE;
                        xQueueSend(xQueueLed, &idle, 0);
                    }
                }
            }
            sample_idx = 0;
        }

        gpio_put(DBG_SENSOR, 0); /* --- WCET end --- */

        vTaskDelayUntil(&xLastWakeTime, xPeriod);
    }
}

/* -------------------------------------------------------------------------- */
/* task_bluetooth                                                              */
/* Receives motion labels and button events, sends commands over UART.        */
/* -------------------------------------------------------------------------- */
static void task_bluetooth(void *param) {
    (void)param;

    /* Init UART0 for HC-06 (or any serial BT module) */
    uart_init(BT_UART_ID, BT_BAUD);
    gpio_set_function(BT_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(BT_RX_PIN, GPIO_FUNC_UART);
    uart_set_hw_flow(BT_UART_ID, false, false);
    uart_set_format(BT_UART_ID, 8, 1, UART_PARITY_NONE);
    ei_printf("[bt] UART0 ready @ %d baud (TX=GP%d, RX=GP%d)\n",
              BT_BAUD, BT_TX_PIN, BT_RX_PIN);

    auto send_cmd = [](char cmd) {
        gpio_put(DBG_BT, 1);
        char buf[2] = {cmd, '\n'};
        uart_write_blocking(BT_UART_ID, (const uint8_t *)buf, 2);
        gpio_put(DBG_BT, 0);
    };

    int last_motion = LABEL_IDLE;

    while (1) {
        int motion;
        int btn;

        /* Check for new motion classification (non-blocking) */
        if (xQueueReceive(xQueueMotion, &motion, 0) == pdTRUE) {
            if (motion != last_motion) {
                last_motion = motion;
                switch (motion) {
                    case LABEL_IDLE:    send_cmd(CMD_IDLE);   break;
                    case LABEL_JUMP:    send_cmd(CMD_JUMP);   break;
                    case LABEL_CROUNCH: send_cmd(CMD_CROUCH); break;
                    case LABEL_LEFT:    send_cmd(CMD_LEFT);   break;
                    case LABEL_RIGHT:   send_cmd(CMD_RIGHT);  break;
                    default:             break;
                }
            }
        }

        /* Check for button press (non-blocking) */
        if (xQueueReceive(xQueueButtons, &btn, 0) == pdTRUE) {
            switch (btn) {
                case BTN_ID_START:    send_cmd(CMD_START);    break;
                case BTN_ID_PAUSE:    send_cmd(CMD_PAUSE);    break;
                case BTN_ID_VOL_UP:   send_cmd(CMD_VOL_UP);   break;
                case BTN_ID_VOL_DOWN: send_cmd(CMD_VOL_DOWN); break;
                default: break;
            }
        }

        /* Block for up to 10ms waiting for new data */
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* -------------------------------------------------------------------------- */
/* task_led                                                                    */
/* Updates RGB LED based on the current motion label.                         */
/* -------------------------------------------------------------------------- */
static void task_led(void *param) {
    (void)param;

    gpio_init(LED_PIN_R); gpio_set_dir(LED_PIN_R, GPIO_OUT);
    gpio_init(LED_PIN_G); gpio_set_dir(LED_PIN_G, GPIO_OUT);
    gpio_init(LED_PIN_B); gpio_set_dir(LED_PIN_B, GPIO_OUT);

    auto set_rgb = [](bool r, bool g, bool b) {
        gpio_put(LED_PIN_R, r);
        gpio_put(LED_PIN_G, g);
        gpio_put(LED_PIN_B, b);
    };

    set_rgb(0, 0, 0);

    while (1) {
        int label;
        if (xQueueReceive(xQueueLed, &label, portMAX_DELAY) == pdTRUE) {
            gpio_put(DBG_LED, 1);
            switch (label) {
                case LABEL_IDLE:    set_rgb(0, 0, 0); break; /* off    */
                case LABEL_JUMP:    set_rgb(0, 0, 1); break; /* blue   = pular  */
                case LABEL_CROUNCH: set_rgb(1, 0, 1); break; /* purple = agachar*/
                case LABEL_LEFT:    set_rgb(0, 1, 0); break; /* green  = esq    */
                case LABEL_RIGHT:   set_rgb(1, 1, 0); break; /* yellow = dir    */
                default:           set_rgb(1, 0, 0); break; /* red = unknown */
            }
            gpio_put(DBG_LED, 0);
        }
    }
}

/* -------------------------------------------------------------------------- */
/* main                                                                        */
/* -------------------------------------------------------------------------- */
int main(void) {
    stdio_init_all();
    sleep_ms(3000); /* aguarda USB CDC conectar antes de qualquer printf */
    printf("=== Skate Controller Boot ===\n");

    /* Debug GPIO pins for oscilloscope WCET measurement */
    const uint dbg_pins[] = {DBG_SENSOR, DBG_BT, DBG_LED};
    for (uint p : dbg_pins) {
        gpio_init(p);
        gpio_set_dir(p, GPIO_OUT);
        gpio_put(p, 0);
    }

    /* Button GPIO with pull-up and interrupt on falling edge */
    const uint btn_pins[] = {BTN_PIN_R, BTN_PIN_G, BTN_PIN_B, (uint)BTN_PIN_VOL_DOWN};
    for (uint p : btn_pins) {
        gpio_init(p);
        gpio_set_dir(p, GPIO_IN);
        gpio_pull_up(p);
        gpio_set_irq_enabled_with_callback(
            p, GPIO_IRQ_EDGE_FALL, true, &gpio_irq_handler);
    }

    /* FreeRTOS queues */
    xQueueMotion  = xQueueCreate(5,  sizeof(int));
    xQueueButtons = xQueueCreate(10, sizeof(int));
    xQueueLed     = xQueueCreate(5,  sizeof(int));

    configASSERT(xQueueMotion);
    configASSERT(xQueueButtons);
    configASSERT(xQueueLed);

    /* Tasks */
    xTaskCreate(task_sensor_ai, "sensor", 16384, NULL, 3, NULL);
    xTaskCreate(task_bluetooth, "bt",      2048, NULL, 2, NULL);
    xTaskCreate(task_led,       "led",     1024, NULL, 1, NULL);

    ei_printf("Skate controller starting...\n");
    vTaskStartScheduler();

    /* Should never reach here */
    while (1) {}
    return 0;
}
