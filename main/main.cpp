/*
 * Skate Controller — Subway Surfers
 * FreeRTOS SMP (dual-core) + Edge Impulse + HC-06 Bluetooth
 *
 * Hardware (Raspberry Pi Pico / RP2040):
 *   MPU6050 IMU  : I2C0  SDA=GP16, SCL=GP17
 *   HC-06 BT     : UART1 TX=GP4 (Pico→HC06), RX=GP5 (HC06→Pico), 115200 baud
 *   HC-06 KEY/EN : GP10  (modo AT — conecte ao pino KEY do módulo)
 *   HC-06 STATE  : GP22  (HIGH quando pareado — opcional)
 *   Botões       : GP18 (Start), GP19 (Pause), GP20 (Vol+), GP21 (Vol-)
 *   LED RGB      : GP6 (R), GP3 (G), GP2 (B)  — active HIGH
 *   Debug WCET   : GP26 (sensor), GP27 (bluetooth), GP28 (led)
 *
 * Tasks e cores:
 *   Core 0 → task_sensor_ai  (amostragem 83 Hz + inferência EI)
 *   Core 1 → task_bluetooth  (envia comandos HC-06)
 *   Core 1 → task_led        (feedback visual RGB)
 *
 * Comandos BT:  'J'=jump  'L'=left  'R'=right  'C'=crouch  'I'=idle
 *               'S'=start 'P'=pause 'U'=vol+   'D'=vol-
 *
 * Ordem das classes do modelo (alfabética, Edge Impulse):
 *   Confirme em ei-model/model-parameters/model_variables.h
 *   ei_classifier_inferencing_categories[]
 */

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "hardware/uart.h"
#include "hardware/gpio.h"

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

#include "hardware/irq.h"

#include "mpu6050.h"
#include "hc06.h"
#include "pins.h"

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Configuração de pinos                                                      */
/* ═══════════════════════════════════════════════════════════════════════════ */

/* MPU6050 — I2C0 */
#define MPU_I2C               i2c0
#define MPU_SDA_PIN           16
#define MPU_SCL_PIN           17
#define MPU_ADDR              MPU6050_I2C_DEFAULT   /* 0x68 */
#define MPU_SENS_2G           16384.0f              /* LSB/g @ ±2g */

/* HC-06 — herdado de hc06.h (HC06_UART_ID, HC06_BAUD_RATE, pinos) */
/* Fila de TX: task_bluetooth deposita bytes; ISR envia via UART     */
static QueueHandle_t xQueueBTTX;

/* Pinos de debug para medir WCET/Jitter no osciloscópio (lab RTOS) */
#define DBG_SENSOR            26   /* toggle durante task_sensor_ai  */
#define DBG_BT                27   /* toggle durante task_bluetooth  */
#define DBG_LED               28   /* toggle durante task_led        */

/* IDs dos botões (valores enviados pela ISR na fila) */
#define BTN_ID_START          0
#define BTN_ID_PAUSE          1
#define BTN_ID_VOL_UP         2
#define BTN_ID_VOL_DOWN       3

/*
 * Labels do modelo — ordem ALFABÉTICA (Edge Impulse ordena assim).
 * Verifique ei_classifier_inferencing_categories[] e ajuste se necessário.
 *   0 = "crouch"  → agachar
 *   1 = "idle"    → parado
 *   2 = "jump"    → pular
 *   3 = "left"    → esquerda
 *   4 = "right"   → direita
 */
#define LABEL_CROUCH          0
#define LABEL_IDLE            1
#define LABEL_JUMP            2
#define LABEL_LEFT            3
#define LABEL_RIGHT           4

/* Comandos enviados via Bluetooth (1 byte + '\n') */
#define CMD_IDLE              'I'
#define CMD_JUMP              'J'
#define CMD_LEFT              'L'
#define CMD_RIGHT             'R'
#define CMD_CROUCH            'C'
#define CMD_START             'S'
#define CMD_PAUSE             'P'
#define CMD_VOL_UP            'U'
#define CMD_VOL_DOWN          'D'

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  EI porting layer                                                           */
/*  Deve ser definido ANTES de incluir os headers do Edge Impulse              */
/* ═══════════════════════════════════════════════════════════════════════════ */
#include "edge-impulse-sdk/dsp/returntypes.h"

void ei_printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}
void  ei_printf_float(float f)  { printf("%f", f); }
void  ei_putchar(char c)        { putchar(c); }
char  ei_getchar(void)          { return (char)getchar_timeout_us(0); }

/* Memória thread-safe: usa heap do FreeRTOS quando o scheduler já rodou */
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
    if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED)
        vPortFree(p);
    else
        free(p);
}

EI_IMPULSE_ERROR ei_run_impulse_check_canceled(void) { return EI_IMPULSE_OK; }
void             ei_serial_set_baudrate(int b)        { (void)b; }
uint64_t         ei_read_timer_ms(void) { return to_ms_since_boot(get_absolute_time()); }
uint64_t         ei_read_timer_us(void) { return to_us_since_boot(get_absolute_time()); }
EI_IMPULSE_ERROR ei_sleep(int32_t ms)  { vTaskDelay(pdMS_TO_TICKS(ms)); return EI_IMPULSE_OK; }

#include "edge-impulse-sdk/classifier/ei_run_classifier.h"
#include "edge-impulse-sdk/dsp/numpy.hpp"

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Driver MPU6050                                                             */
/* ═══════════════════════════════════════════════════════════════════════════ */

static void mpu_write(uint8_t reg, uint8_t val) {
    uint8_t buf[2] = {reg, val};
    i2c_write_blocking(MPU_I2C, MPU_ADDR, buf, 2, false);
}

static bool mpu_init(void) {
    i2c_init(MPU_I2C, 400 * 1000);
    gpio_set_function(MPU_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(MPU_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(MPU_SDA_PIN);
    gpio_pull_up(MPU_SCL_PIN);

    mpu_write(MPUREG_PWR_MGMT_1, 0x00);   /* sai do sleep mode */
    sleep_ms(100);

    uint8_t reg = MPUREG_WHOAMI, id = 0;
    i2c_write_blocking(MPU_I2C, MPU_ADDR, &reg, 1, true);
    i2c_read_blocking (MPU_I2C, MPU_ADDR, &id,  1, false);
    if (id != 0x68) return false;

    mpu_write(MPUREG_ACCEL_CONFIG, 0x00);  /* range ±2g  */
    mpu_write(MPUREG_SMPLRT_DIV,   9);     /* 100 Hz     */
    return true;
}

static void mpu_read_accel(float *ax, float *ay, float *az) {
    uint8_t reg = MPUREG_ACCEL_XOUT_H, raw[6];
    i2c_write_blocking(MPU_I2C, MPU_ADDR, &reg, 1, true);
    i2c_read_blocking (MPU_I2C, MPU_ADDR, raw, 6, false);

    *ax = (int16_t)((raw[0] << 8) | raw[1]) / MPU_SENS_2G * 9.80665f;
    *ay = (int16_t)((raw[2] << 8) | raw[3]) / MPU_SENS_2G * 9.80665f;
    *az = (int16_t)((raw[4] << 8) | raw[5]) / MPU_SENS_2G * 9.80665f;
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  FreeRTOS — filas e handles de task                                         */
/* ═══════════════════════════════════════════════════════════════════════════ */

static QueueHandle_t xQueueMotion;    /* sensor_ai  → bluetooth  (int label) */
static QueueHandle_t xQueueButtons;   /* ISR        → bluetooth  (int btn)   */
static QueueHandle_t xQueueLed;       /* sensor_ai  → led        (int label) */

static TaskHandle_t  hSensor = NULL;
static TaskHandle_t  hBT     = NULL;
static TaskHandle_t  hLed    = NULL;

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  ISR — botões                                                               */
/* ═══════════════════════════════════════════════════════════════════════════ */

static void gpio_irq_handler(uint gpio, uint32_t events) {
    if (!(events & GPIO_IRQ_EDGE_FALL)) return;

    int id = -1;
    if      (gpio == (uint)BTN_PIN_RED)    id = BTN_ID_START;
    else if (gpio == (uint)BTN_PIN_GREEN)  id = BTN_ID_PAUSE;
    else if (gpio == (uint)BTN_PIN_BLUE)   id = BTN_ID_VOL_UP;
    else if (gpio == (uint)BTN_PIN_YELLOW) id = BTN_ID_VOL_DOWN;
    if (id < 0) return;

    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(xQueueButtons, &id, &woken);
    portYIELD_FROM_ISR(woken);
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  task_sensor_ai  (Core 0)                                                   */
/*  Amostra MPU6050 a 83 Hz, preenche buffer EI, executa inferência.           */
/*  Pino DBG_SENSOR (GP26) fica HIGH durante todo o corpo do tick —            */
/*  o pico de largura quando ocorre a inferência é o WCET da task.             */
/* ═══════════════════════════════════════════════════════════════════════════ */

static void task_sensor_ai(void *param) {
    (void)param;

    if (!mpu_init()) {
        printf("[sensor] MPU6050 FALHOU (SDA=GP%d, SCL=GP%d, addr=0x%02X)\n",
               MPU_SDA_PIN, MPU_SCL_PIN, MPU_ADDR);
        vTaskDelete(NULL);
        return;
    }
    printf("[sensor] MPU6050 OK — iniciando amostragem a %.0f Hz\n",
           1000.0f / EI_CLASSIFIER_INTERVAL_MS);

    static float ei_buf[EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE];
    int idx = 0;

    TickType_t last = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS((uint32_t)EI_CLASSIFIER_INTERVAL_MS);

    while (1) {
        gpio_put(DBG_SENSOR, 1);                   /* ← WCET start */

        float ax, ay, az;
        mpu_read_accel(&ax, &ay, &az);

        ei_buf[idx * EI_CLASSIFIER_RAW_SAMPLES_PER_FRAME + 0] = ax;
        ei_buf[idx * EI_CLASSIFIER_RAW_SAMPLES_PER_FRAME + 1] = ay;
        ei_buf[idx * EI_CLASSIFIER_RAW_SAMPLES_PER_FRAME + 2] = az;
        idx++;

        if (idx >= EI_CLASSIFIER_RAW_SAMPLE_COUNT) {
            /* Buffer cheio → inferência (este tick tem WCET máximo) */
            signal_t sig;
            if (numpy::signal_from_buffer(ei_buf, EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE, &sig) == 0) {
                ei_impulse_result_t res = {0};
                if (run_classifier(&sig, &res, false) == EI_IMPULSE_OK) {

                    /* Encontra label com maior confiança */
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

        gpio_put(DBG_SENSOR, 0);                   /* ← WCET end */
        vTaskDelayUntil(&last, period);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  task_tx  (Core 1)                                                          */
/*  Consome bytes da xQueueBTTX e os envia pelo UART do HC-06.                 */
/*  Separar TX em task própria é o padrão do lab: evita bloquear task_bt.      */
/* ═══════════════════════════════════════════════════════════════════════════ */

static void task_tx(void *param) {
    (void)param;
    uint8_t ch;
    while (1) {
        if (xQueueReceive(xQueueBTTX, &ch, portMAX_DELAY) == pdTRUE) {
            gpio_put(DBG_BT, 1);
            uart_putc_raw(HC06_UART_ID, ch);
            gpio_put(DBG_BT, 0);
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  task_bluetooth  (Core 1)                                                   */
/*  Recebe labels de movimento e eventos de botão, deposita bytes na fila TX.  */
/* ═══════════════════════════════════════════════════════════════════════════ */

static void task_bluetooth(void *param) {
    (void)param;

    /* Deposita cmd + '\n' na fila de TX */
    auto send = [](char cmd) {
        uint8_t bytes[2] = {(uint8_t)cmd, '\n'};
        xQueueSend(xQueueBTTX, &bytes[0], 0);
        xQueueSend(xQueueBTTX, &bytes[1], 0);
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
                case BTN_ID_START:    send(CMD_START);    break;
                case BTN_ID_PAUSE:    send(CMD_PAUSE);    break;
                case BTN_ID_VOL_UP:   send(CMD_VOL_UP);   break;
                case BTN_ID_VOL_DOWN: send(CMD_VOL_DOWN); break;
                default: break;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  task_led  (Core 1)                                                         */
/*  Atualiza LED RGB conforme o movimento detectado.                           */
/* ═══════════════════════════════════════════════════════════════════════════ */

static void task_led(void *param) {
    (void)param;

    for (int p : {LED_PIN_R, LED_PIN_G, LED_PIN_B}) {
        gpio_init(p);
        gpio_set_dir(p, GPIO_OUT);
        gpio_put(p, 0);
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
                case LABEL_IDLE:   rgb(0,0,0); break;  /* apagado */
                case LABEL_JUMP:   rgb(0,0,1); break;  /* azul    */
                case LABEL_LEFT:   rgb(0,1,0); break;  /* verde   */
                case LABEL_RIGHT:  rgb(1,1,0); break;  /* amarelo */
                case LABEL_CROUCH: rgb(1,0,1); break;  /* roxo    */
                default:           rgb(1,0,0); break;  /* vermelho = erro */
            }
            gpio_put(DBG_LED, 0);
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  main                                                                       */
/* ═══════════════════════════════════════════════════════════════════════════ */

int main(void) {
    stdio_init_all();
    sleep_ms(2000);  /* aguarda USB conectar */
    printf("\n=== Skate Controller — Dual-Core FreeRTOS ===\n");
    printf("Core 0: sensor_ai | Core 1: bluetooth + led\n\n");

    /* ── HC-06: inicializa UART e configura módulo via AT commands ─────────
     * Deve acontecer ANTES do scheduler pois usa sleep_ms internamente.
     * hc06_config() detecta baud (9600 ou 115200), configura nome/PIN e
     * garante que o módulo opera a 115200 no fim.                          */
    uart_init(HC06_UART_ID, HC06_BAUD_RATE);
    gpio_set_function(HC06_TX_PIN, UART_FUNCSEL_NUM(HC06_UART_ID, HC06_TX_PIN));
    gpio_set_function(HC06_RX_PIN, UART_FUNCSEL_NUM(HC06_UART_ID, HC06_RX_PIN));
    uart_set_baudrate(HC06_UART_ID, HC06_BAUD_RATE);
    uart_set_hw_flow(HC06_UART_ID, false, false);
    uart_set_format(HC06_UART_ID, 8, 1, UART_PARITY_NONE);
    uart_set_fifo_enabled(HC06_UART_ID, false);  /* byte a byte, sem buffer */

    hc06_config("SKATE-CTRL", "1234");
    printf("[main] HC-06 pronto @ 115200 baud\n\n");

    /* ── Pinos de debug WCET (osciloscópio) ──────────────────────────────── */
    for (uint p : {(uint)DBG_SENSOR, (uint)DBG_BT, (uint)DBG_LED}) {
        gpio_init(p); gpio_set_dir(p, GPIO_OUT); gpio_put(p, 0);
    }

    /* ── Botões com pull-up e interrupção na borda de descida ────────────── */
    for (uint p : {(uint)BTN_PIN_RED, (uint)BTN_PIN_GREEN,
                   (uint)BTN_PIN_BLUE, (uint)BTN_PIN_YELLOW}) {
        gpio_init(p);
        gpio_set_dir(p, GPIO_IN);
        gpio_pull_up(p);
        gpio_set_irq_enabled_with_callback(p, GPIO_IRQ_EDGE_FALL, true, &gpio_irq_handler);
    }

    /* ── Filas ───────────────────────────────────────────────────────────── */
    xQueueMotion  = xQueueCreate(5,   sizeof(int));
    xQueueButtons = xQueueCreate(10,  sizeof(int));
    xQueueLed     = xQueueCreate(5,   sizeof(int));
    xQueueBTTX    = xQueueCreate(256, sizeof(uint8_t));
    configASSERT(xQueueMotion && xQueueButtons && xQueueLed && xQueueBTTX);

    /* ── Criação das tasks ───────────────────────────────────────────────── */
    TaskHandle_t hTX = NULL;
    xTaskCreate(task_sensor_ai, "sensor", 16384, NULL, 3, &hSensor);
    xTaskCreate(task_bluetooth, "bt",      2048, NULL, 2, &hBT);
    xTaskCreate(task_tx,        "tx",       512, NULL, 2, &hTX);
    xTaskCreate(task_led,       "led",     1024, NULL, 1, &hLed);

    /*
     * Afinidade de core (SMP — dual-core):
     *   Core 0 (bit 0): task_sensor_ai — crítica em tempo real (83 Hz + IA)
     *   Core 1 (bit 1): task_bluetooth + task_led — I/O bound, orientadas a eventos
     *
     * Isso evita que a inferência da IA interfira no envio Bluetooth e vice-versa.
     */
    vTaskCoreAffinitySet(hSensor, (1 << 0));   /* Core 0: sensor + IA    */
    vTaskCoreAffinitySet(hBT,     (1 << 1));   /* Core 1: bluetooth ctrl */
    vTaskCoreAffinitySet(hTX,     (1 << 1));   /* Core 1: bluetooth TX   */
    vTaskCoreAffinitySet(hLed,    (1 << 1));   /* Core 1: led            */

    printf("Tasks criadas — iniciando scheduler (SMP: 2 cores)\n");
    vTaskStartScheduler();

    while (1) {}
    return 0;
}
