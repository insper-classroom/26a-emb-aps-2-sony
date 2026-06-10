/*
 * Skate Controller — Subway Surfers
 * FreeRTOS SMP (dual-core) + Edge Impulse + HC-06 Bluetooth
 *
 * Hardware (Raspberry Pi Pico / RP2040):
 *   MPU6050      : I2C0  SDA=GP16, SCL=GP17
 *   HC-06 TXD    : GP5  (HC-06 transmite → Pico recebe)
 *   HC-06 RXD    : GP4  (HC-06 recebe  ← Pico transmite)
 *   HC-06 VCC    : 3.3V
 *   HC-06 GND    : GND
 *   HC-06 STATE  : GP22 (opcional — HIGH quando pareado)
 *   Botões       : GP18 (Start), GP19 (Pause), GP20 (Vol+), GP21 (Vol-)
 *   LED RGB      : GP6 (R), GP3 (G), GP2 (B)
 *   Debug WCET   : GP26 (sensor), GP27 (bt), GP28 (led)
 *
 * Tasks / cores:
 *   Core 0 → task_sensor_ai  (83 Hz + inferência Edge Impulse)
 *   Core 1 → task_bluetooth  (decide o comando a enviar)
 *   Core 1 → task_tx         (envia bytes pelo UART)
 *   Core 1 → task_led        (feedback RGB)
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

/* ── Bluetooth (HC-06, UART1) ─────────────────────────────────────────────── */
#define BT_UART     uart1
#define BT_TX_PIN   4        /* Pico TX → HC-06 RXD */
#define BT_RX_PIN   5        /* Pico RX ← HC-06 TXD */
#define BT_BAUD     9600

/* ── MPU6050 (I2C0) ──────────────────────────────────────────────────────── */
#define MPU_I2C     i2c0
#define MPU_SDA     16
#define MPU_SCL     17
#define MPU_ADDR    MPU6050_I2C_DEFAULT  /* 0x68 */
#define MPU_SENS    16384.0f             /* LSB/g @ ±2g */

/* ── Debug WCET (osciloscópio) ───────────────────────────────────────────── */
#define DBG_SENSOR  26
#define DBG_BT      27
#define DBG_LED     28

/* ── Botões ──────────────────────────────────────────────────────────────── */
#define BTN_START    0
#define BTN_PAUSE    1
#define BTN_VOL_UP   2
#define BTN_VOL_DOWN 3

/* ── Labels do modelo (ordem ALFABÉTICA — Edge Impulse) ──────────────────── */
#define LABEL_CROUCH  0
#define LABEL_IDLE    1
#define LABEL_JUMP    2
#define LABEL_LEFT    3
#define LABEL_RIGHT   4

/* ── Comandos Bluetooth ──────────────────────────────────────────────────── */
#define CMD_IDLE    'I'
#define CMD_JUMP    'J'
#define CMD_LEFT    'L'
#define CMD_RIGHT   'R'
#define CMD_CROUCH  'C'
#define CMD_START   'S'
#define CMD_PAUSE   'P'
#define CMD_VOLUP   'U'
#define CMD_VOLDN   'D'

/* ══════════════════════════════════════════════════════════════════════════ */
/*  EI porting layer — deve vir ANTES dos includes do Edge Impulse           */
/* ══════════════════════════════════════════════════════════════════════════ */
#include "edge-impulse-sdk/dsp/returntypes.h"

void ei_printf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
}
void  ei_printf_float(float f) { printf("%f", f); }
void  ei_putchar(char c)       { putchar(c); }
char  ei_getchar(void)         { return (char)getchar_timeout_us(0); }

void *ei_malloc(size_t sz) {
    return (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED)
           ? pvPortMalloc(sz) : malloc(sz);
}
void *ei_calloc(size_t n, size_t sz) {
    void *p = ei_malloc(n * sz);
    if (p) memset(p, 0, n * sz);
    return p;
}
void ei_free(void *p) {
    if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) vPortFree(p);
    else free(p);
}

EI_IMPULSE_ERROR ei_run_impulse_check_canceled(void) { return EI_IMPULSE_OK; }
void             ei_serial_set_baudrate(int b)        { (void)b; }
uint64_t         ei_read_timer_ms(void) { return to_ms_since_boot(get_absolute_time()); }
uint64_t         ei_read_timer_us(void) { return to_us_since_boot(get_absolute_time()); }
EI_IMPULSE_ERROR ei_sleep(int32_t ms)  { vTaskDelay(pdMS_TO_TICKS(ms)); return EI_IMPULSE_OK; }

#include "edge-impulse-sdk/classifier/ei_run_classifier.h"
#include "edge-impulse-sdk/dsp/numpy.hpp"

/* ══════════════════════════════════════════════════════════════════════════ */
/*  MPU6050 driver                                                            */
/* ══════════════════════════════════════════════════════════════════════════ */

static void mpu_write(uint8_t reg, uint8_t val) {
    uint8_t buf[2] = {reg, val};
    i2c_write_blocking(MPU_I2C, MPU_ADDR, buf, 2, false);
}

static bool mpu_init(void) {
    i2c_init(MPU_I2C, 400 * 1000);
    gpio_set_function(MPU_SDA, GPIO_FUNC_I2C);
    gpio_set_function(MPU_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(MPU_SDA);
    gpio_pull_up(MPU_SCL);

    mpu_write(MPUREG_PWR_MGMT_1, 0x00);
    sleep_ms(100);

    uint8_t reg = MPUREG_WHOAMI, id = 0;
    i2c_write_blocking(MPU_I2C, MPU_ADDR, &reg, 1, true);
    i2c_read_blocking (MPU_I2C, MPU_ADDR, &id,  1, false);
    if (id != 0x68) return false;

    mpu_write(MPUREG_ACCEL_CONFIG, 0x00);
    mpu_write(MPUREG_SMPLRT_DIV,   9);
    return true;
}

static void mpu_read(float *ax, float *ay, float *az) {
    uint8_t reg = MPUREG_ACCEL_XOUT_H, raw[6];
    i2c_write_blocking(MPU_I2C, MPU_ADDR, &reg, 1, true);
    i2c_read_blocking (MPU_I2C, MPU_ADDR, raw, 6, false);
    *ax = (int16_t)((raw[0]<<8)|raw[1]) / MPU_SENS * 9.80665f;
    *ay = (int16_t)((raw[2]<<8)|raw[3]) / MPU_SENS * 9.80665f;
    *az = (int16_t)((raw[4]<<8)|raw[5]) / MPU_SENS * 9.80665f;
}

/* ══════════════════════════════════════════════════════════════════════════ */
/*  FreeRTOS — filas e handles                                                */
/* ══════════════════════════════════════════════════════════════════════════ */

static QueueHandle_t xQueueMotion;   /* sensor_ai → bluetooth */
static QueueHandle_t xQueueButtons;  /* ISR       → bluetooth */
static QueueHandle_t xQueueLed;      /* sensor_ai → led       */
static QueueHandle_t xQueueTX;       /* bluetooth → task_tx   */

static TaskHandle_t hSensor, hBT, hTX, hLed;

/* ══════════════════════════════════════════════════════════════════════════ */
/*  ISR — botões                                                              */
/* ══════════════════════════════════════════════════════════════════════════ */

static void gpio_irq_handler(uint gpio, uint32_t events) {
    if (!(events & GPIO_IRQ_EDGE_FALL)) return;
    int id = -1;
    if      (gpio == (uint)BTN_PIN_RED)    id = BTN_START;
    else if (gpio == (uint)BTN_PIN_GREEN)  id = BTN_PAUSE;
    else if (gpio == (uint)BTN_PIN_BLUE)   id = BTN_VOL_UP;
    else if (gpio == (uint)BTN_PIN_YELLOW) id = BTN_VOL_DOWN;
    if (id < 0) return;
    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(xQueueButtons, &id, &woken);
    portYIELD_FROM_ISR(woken);
}

/* ══════════════════════════════════════════════════════════════════════════ */
/*  task_sensor_ai  —  Core 0                                                 */
/*  Lê MPU6050 a 83 Hz, acumula buffer e roda inferência Edge Impulse.        */
/* ══════════════════════════════════════════════════════════════════════════ */

static void task_sensor_ai(void *p) {
    (void)p;
    if (!mpu_init()) {
        printf("[sensor] MPU6050 nao encontrado!\n");
        vTaskDelete(NULL);
        return;
    }
    printf("[sensor] MPU6050 OK\n");

    static float buf[EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE];
    int idx = 0;
    TickType_t last = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS((uint32_t)EI_CLASSIFIER_INTERVAL_MS);

    while (1) {
        gpio_put(DBG_SENSOR, 1);

        float ax, ay, az;
        mpu_read(&ax, &ay, &az);
        buf[idx * EI_CLASSIFIER_RAW_SAMPLES_PER_FRAME + 0] = ax;
        buf[idx * EI_CLASSIFIER_RAW_SAMPLES_PER_FRAME + 1] = ay;
        buf[idx * EI_CLASSIFIER_RAW_SAMPLES_PER_FRAME + 2] = az;
        idx++;

        if (idx >= EI_CLASSIFIER_RAW_SAMPLE_COUNT) {
            signal_t sig;
            if (numpy::signal_from_buffer(buf, EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE, &sig) == 0) {
                ei_impulse_result_t res = {0};
                if (run_classifier(&sig, &res, false) == EI_IMPULSE_OK) {
                    int best = 0;
                    for (int i = 1; i < EI_CLASSIFIER_LABEL_COUNT; i++)
                        if (res.classification[i].value > res.classification[best].value)
                            best = i;
                    if (res.classification[best].value >= EI_CLASSIFIER_THRESHOLD) {
                        xQueueSend(xQueueMotion, &best, 0);
                        xQueueSend(xQueueLed,    &best, 0);
                    } else {
                        int idle = LABEL_IDLE;
                        xQueueSend(xQueueLed, &idle, 0);
                    }
                }
            }
            idx = 0;
        }

        gpio_put(DBG_SENSOR, 0);
        vTaskDelayUntil(&last, period);
    }
}

/* ══════════════════════════════════════════════════════════════════════════ */
/*  task_tx  —  Core 1                                                        */
/*  Consome bytes de xQueueTX e os envia pelo UART do HC-06.                  */
/* ══════════════════════════════════════════════════════════════════════════ */

static void task_tx(void *p) {
    (void)p;
    uint8_t ch;
    while (1) {
        if (xQueueReceive(xQueueTX, &ch, portMAX_DELAY) == pdTRUE) {
            gpio_put(DBG_BT, 1);
            uart_putc_raw(BT_UART, ch);
            gpio_put(DBG_BT, 0);
        }
    }
}

/* ══════════════════════════════════════════════════════════════════════════ */
/*  task_bluetooth  —  Core 1                                                 */
/*  Decide qual comando enviar e deposita na fila de TX.                      */
/* ══════════════════════════════════════════════════════════════════════════ */

static void task_bluetooth(void *p) {
    (void)p;

    auto send = [](char cmd) {
        uint8_t msg[2] = {(uint8_t)cmd, '\n'};
        xQueueSend(xQueueTX, &msg[0], 0);
        xQueueSend(xQueueTX, &msg[1], 0);
    };

    int last_label = LABEL_IDLE;

    while (1) {
        int label;
        if (xQueueReceive(xQueueMotion, &label, 0) == pdTRUE) {
            if (label != last_label) {
                last_label = label;
                switch (label) {
                    case LABEL_IDLE:   send(CMD_IDLE);   break;
                    case LABEL_JUMP:   send(CMD_JUMP);   break;
                    case LABEL_LEFT:   send(CMD_LEFT);   break;
                    case LABEL_RIGHT:  send(CMD_RIGHT);  break;
                    case LABEL_CROUCH: send(CMD_CROUCH); break;
                    default: break;
                }
            }
        }

        int btn;
        if (xQueueReceive(xQueueButtons, &btn, 0) == pdTRUE) {
            switch (btn) {
                case BTN_START:    send(CMD_START);  break;
                case BTN_PAUSE:    send(CMD_PAUSE);  break;
                case BTN_VOL_UP:   send(CMD_VOLUP);  break;
                case BTN_VOL_DOWN: send(CMD_VOLDN);  break;
                default: break;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* ══════════════════════════════════════════════════════════════════════════ */
/*  task_led  —  Core 1                                                       */
/*  Atualiza LED RGB conforme o movimento classificado.                       */
/* ══════════════════════════════════════════════════════════════════════════ */

static void task_led(void *p) {
    (void)p;

    for (int pin : {LED_PIN_R, LED_PIN_G, LED_PIN_B}) {
        gpio_init(pin); gpio_set_dir(pin, GPIO_OUT); gpio_put(pin, 0);
    }

    auto rgb = [](bool r, bool g, bool b) {
        gpio_put(LED_PIN_R, r);
        gpio_put(LED_PIN_G, g);
        gpio_put(LED_PIN_B, b);
    };

    while (1) {
        int label;
        if (xQueueReceive(xQueueLed, &label, portMAX_DELAY) == pdTRUE) {
            gpio_put(DBG_LED, 1);
            switch (label) {
                case LABEL_IDLE:   rgb(0,0,0); break;
                case LABEL_JUMP:   rgb(0,0,1); break;  /* azul    */
                case LABEL_LEFT:   rgb(0,1,0); break;  /* verde   */
                case LABEL_RIGHT:  rgb(1,1,0); break;  /* amarelo */
                case LABEL_CROUCH: rgb(1,0,1); break;  /* roxo    */
                default:           rgb(1,0,0); break;  /* vermelho = desconhecido */
            }
            gpio_put(DBG_LED, 0);
        }
    }
}

/* ══════════════════════════════════════════════════════════════════════════ */
/*  main                                                                      */
/* ══════════════════════════════════════════════════════════════════════════ */

int main(void) {
    stdio_init_all();
    sleep_ms(2000);
    printf("\n=== Skate Controller ===\n");

    /* HC-06 UART */
    uart_init(BT_UART, BT_BAUD);
    gpio_set_function(BT_TX_PIN, UART_FUNCSEL_NUM(BT_UART, BT_TX_PIN));
    gpio_set_function(BT_RX_PIN, UART_FUNCSEL_NUM(BT_UART, BT_RX_PIN));
    uart_set_hw_flow(BT_UART, false, false);
    uart_set_format(BT_UART, 8, 1, UART_PARITY_NONE);
    uart_set_fifo_enabled(BT_UART, false);
    printf("[bt] UART1 @ %d baud — GP%d/GP%d\n", BT_BAUD, BT_TX_PIN, BT_RX_PIN);

    /* Debug GPIO */
    for (uint pin : {(uint)DBG_SENSOR, (uint)DBG_BT, (uint)DBG_LED}) {
        gpio_init(pin); gpio_set_dir(pin, GPIO_OUT); gpio_put(pin, 0);
    }

    /* Botões */
    for (uint pin : {(uint)BTN_PIN_RED, (uint)BTN_PIN_GREEN,
                     (uint)BTN_PIN_BLUE, (uint)BTN_PIN_YELLOW}) {
        gpio_init(pin);
        gpio_set_dir(pin, GPIO_IN);
        gpio_pull_up(pin);
        gpio_set_irq_enabled_with_callback(pin, GPIO_IRQ_EDGE_FALL, true, &gpio_irq_handler);
    }

    /* Filas */
    xQueueMotion  = xQueueCreate(5,   sizeof(int));
    xQueueButtons = xQueueCreate(10,  sizeof(int));
    xQueueLed     = xQueueCreate(5,   sizeof(int));
    xQueueTX      = xQueueCreate(256, sizeof(uint8_t));
    configASSERT(xQueueMotion && xQueueButtons && xQueueLed && xQueueTX);

    /* Tasks */
    xTaskCreate(task_sensor_ai, "sensor", 16384, NULL, 3, &hSensor);
    xTaskCreate(task_bluetooth, "bt",      2048, NULL, 2, &hBT);
    xTaskCreate(task_tx,        "tx",       512, NULL, 2, &hTX);
    xTaskCreate(task_led,       "led",     1024, NULL, 1, &hLed);

    /* Afinidade de core — SMP dual-core */
    vTaskCoreAffinitySet(hSensor, (1 << 0));  /* Core 0: sensor + IA */
    vTaskCoreAffinitySet(hBT,     (1 << 1));  /* Core 1: bluetooth   */
    vTaskCoreAffinitySet(hTX,     (1 << 1));  /* Core 1: uart tx     */
    vTaskCoreAffinitySet(hLed,    (1 << 1));  /* Core 1: led         */

    printf("Iniciando scheduler...\n");
    vTaskStartScheduler();

    while (1) {}
    return 0;
}
