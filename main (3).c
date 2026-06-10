#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

#include "pico/stdlib.h"
#include "hardware/uart.h"
#include "hardware/irq.h"
#include "hardware/adc.h"
#include "hardware/pwm.h"
#include "hardware/i2c.h"

#include "hc06.h"
#include "pins.h"
#include "ssd1306/ssd1306.h"

// ---------------------------------------------------------------------------
// Configuração ADC / mouse
// ---------------------------------------------------------------------------
#define ADC_INPUT_X      1   // GPIO27 = ADC1
#define ADC_INPUT_Y      0   // GPIO26 = ADC0
#define ADC_MAX_VALUE    4095
#define CALIB_SAMPLES    32
#define MOVING_AVG_SIZE  10
#define DEAD_ZONE_X      1
#define DEAD_ZONE_Y      5
#define MOUSE_SPEED_X    25
#define MOUSE_SPEED_Y    15

// ---------------------------------------------------------------------------
// Configuração PWM do LED de status
// ---------------------------------------------------------------------------
#define PWM_WRAP         1000
#define PWM_FADE_STEP    10
#define PWM_FADE_DELAY   10   // ms por passo de fade

// ---------------------------------------------------------------------------
// Nome do dispositivo BT
// ---------------------------------------------------------------------------
#define HC06_NAME "MOUSE-BT"

// ---------------------------------------------------------------------------
// Queues e handles globais
// ---------------------------------------------------------------------------
typedef struct { int axis; int val; } adc_t;

static QueueHandle_t xQueuePIN;   // char[5]: novo PIN gerado pelo botão
static QueueHandle_t xQueueRX;    // uint8_t: bytes recebidos do HC-06 via ISR
static QueueHandle_t xQueueTX;    // uint8_t: bytes a enviar para o HC-06
static QueueHandle_t xQueueADC;   // adc_t:   leituras do joystick

static SemaphoreHandle_t xAdcMutex;
static TaskHandle_t xTaskOLED;

static ssd1306_t oled;
static char current_pin[5] = "----";

// ---------------------------------------------------------------------------
// UART ISR — alimenta xQueueRX
// ---------------------------------------------------------------------------
static void uart_rx_handler(void) {
    uint8_t ch = uart_getc(HC06_UART_ID);
    xQueueSendFromISR(xQueueRX, &ch, NULL);
}

// ---------------------------------------------------------------------------
// Inicialização da UART do HC-06
// ---------------------------------------------------------------------------
static void init_uart_hc06(void) {
    uart_init(HC06_UART_ID, HC06_BAUD_RATE);
    gpio_set_function(HC06_TX_PIN, UART_FUNCSEL_NUM(HC06_UART_ID, HC06_TX_PIN));
    gpio_set_function(HC06_RX_PIN, UART_FUNCSEL_NUM(HC06_UART_ID, HC06_RX_PIN));
    uart_set_baudrate(HC06_UART_ID, HC06_BAUD_RATE);
    uart_set_hw_flow(HC06_UART_ID, false, false);
    uart_set_format(HC06_UART_ID, 8, 1, UART_PARITY_NONE);
}

static void init_uart_irq(void) {
    uart_set_fifo_enabled(HC06_UART_ID, false);
    int UART_IRQ = HC06_UART_ID == uart0 ? UART0_IRQ : UART1_IRQ;
    irq_set_enabled(UART_IRQ, false);
    irq_set_exclusive_handler(UART_IRQ, uart_rx_handler);
    irq_set_enabled(UART_IRQ, true);
    uart_set_irq_enables(HC06_UART_ID, true, false);
}

static void rearm_uart_irq(void) {
    int UART_IRQ = HC06_UART_ID == uart0 ? UART0_IRQ : UART1_IRQ;
    irq_set_enabled(UART_IRQ, false);
    irq_set_enabled(UART_IRQ, true);
    uart_set_irq_enables(HC06_UART_ID, true, false);
}

// ---------------------------------------------------------------------------
// Helpers ADC (do Lab 6)
// ---------------------------------------------------------------------------
static int read_adc_channel(int input) {
    adc_select_input(input);
    adc_read();
    sleep_us(5);
    return adc_read();
}

static int average_adc_channel(int input, int samples) {
    int sum = 0;
    for (int i = 0; i < samples; i++) {
        sum += read_adc_channel(input);
        sleep_us(200);
    }
    return sum / samples;
}

static int moving_average(int *buffer, int new_val, int *index, int size) {
    buffer[*index] = new_val;
    *index = (*index + 1) % size;
    int sum = 0;
    for (int i = 0; i < size; i++) sum += buffer[i];
    return sum / size;
}

static int scale_adc(int raw, int center, int speed, int dead_zone) {
    int centered = raw - center;
    int range = (centered >= 0) ? (ADC_MAX_VALUE - center) : center;
    if (range <= 0) range = 1;
    int scaled = (centered * speed) / range;
    if (scaled >  speed) scaled =  speed;
    if (scaled < -speed) scaled = -speed;
    if (scaled > -dead_zone && scaled < dead_zone) scaled = 0;
    return scaled;
}

// ---------------------------------------------------------------------------
// Helper PWM — configura um pino como saída PWM com wrap fixo
// ---------------------------------------------------------------------------
static uint pwm_init_pin(uint gpio) {
    gpio_set_function(gpio, GPIO_FUNC_PWM);
    uint slice = pwm_gpio_to_slice_num(gpio);
    pwm_set_wrap(slice, PWM_WRAP);
    pwm_set_gpio_level(gpio, 0);
    pwm_set_enabled(slice, true);
    return slice;
}

// ---------------------------------------------------------------------------
// Helper OLED — redesenha a tela com PIN e status
// ---------------------------------------------------------------------------
static void oled_update(bool connected) {
    char line[32];
    ssd1306_clear(&oled);
    ssd1306_draw_string(&oled, 0, 0, 1, "MOUSE-BT");
    snprintf(line, sizeof(line), "PIN: %s", current_pin);
    ssd1306_draw_string(&oled, 0, 12, 1, line);
    ssd1306_draw_string(&oled, 0, 24, 1, connected ? "Status: CONECTADO" : "Status: AGUARDANDO");
    ssd1306_show(&oled);
}

// ---------------------------------------------------------------------------
// task_button — debounce + geração de PIN randômico (padrão APS1)
// ---------------------------------------------------------------------------
static void task_button(void *p) {
    gpio_init(BTN_PIN);
    gpio_set_dir(BTN_PIN, GPIO_IN);
    gpio_pull_up(BTN_PIN);

    bool last = true;
    char pin_str[5];

    for (;;) {
        bool cur = gpio_get(BTN_PIN);
        if (last && !cur) {
            // borda de descida → debounce
            vTaskDelay(pdMS_TO_TICKS(50));
            if (!gpio_get(BTN_PIN)) {
                srand(time_us_32());
                int pin_num = (rand() % 9000) + 1000;
                snprintf(pin_str, sizeof(pin_str), "%04d", pin_num);
                xQueueOverwrite(xQueuePIN, pin_str);
            }
        }
        last = cur;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ---------------------------------------------------------------------------
// task_hc06_config — reconfigura HC-06 com novo PIN e notifica OLED
// ---------------------------------------------------------------------------
static void task_hc06_config(void *p) {
    xTaskNotify(xTaskOLED, 0, eNoAction);

    char pin_str[5];
    for (;;) {
        if (xQueueReceive(xQueuePIN, pin_str, portMAX_DELAY) == pdTRUE) {
            memcpy(current_pin, pin_str, 5);
            hc06_config(HC06_NAME, pin_str);
            rearm_uart_irq();
            xTaskNotify(xTaskOLED, 0, eNoAction);
        }
    }
}

// ---------------------------------------------------------------------------
// task_oled — atualiza display; acorda via notificação ou a cada 500 ms
// ---------------------------------------------------------------------------
static void task_oled(void *p) {
    // Inicializa I2C1
    i2c_init(i2c1, 400000);
    gpio_set_function(OLED_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(OLED_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(OLED_SDA_PIN);
    gpio_pull_up(OLED_SCL_PIN);

    ssd1306_init(&oled, 128, 32, 0x3C, i2c1);
    ssd1306_clear(&oled);
    ssd1306_show(&oled);

    for (;;) {
        xTaskNotifyWait(0, 0, NULL, pdMS_TO_TICKS(500));
        bool connected = gpio_get(HC06_STATE_PIN);
        oled_update(connected);
    }
}

// ---------------------------------------------------------------------------
// task_led_status — fade PWM enquanto desconectado, sólido quando conectado
// ---------------------------------------------------------------------------
static void task_led_status(void *p) {
    gpio_init(HC06_STATE_PIN);
    gpio_set_dir(HC06_STATE_PIN, GPIO_IN);

    pwm_init_pin(LED_STATUS_PIN);

    int level = 0;
    int dir = PWM_FADE_STEP;

    for (;;) {
        bool connected = gpio_get(HC06_STATE_PIN);
        if (connected) {
            pwm_set_gpio_level(LED_STATUS_PIN, PWM_WRAP);
        } else {
            pwm_set_gpio_level(LED_STATUS_PIN, level);
            level += dir;
            if (level >= PWM_WRAP) { level = PWM_WRAP; dir = -PWM_FADE_STEP; }
            if (level <= 0)        { level = 0;        dir =  PWM_FADE_STEP; }
        }
        vTaskDelay(pdMS_TO_TICKS(PWM_FADE_DELAY));
    }
}

// ---------------------------------------------------------------------------
// task_bt_tx — consome xQueueTX e envia ao HC-06
// ---------------------------------------------------------------------------
static void task_bt_tx(void *p) {
    uint8_t ch;
    for (;;) {
        if (xQueueReceive(xQueueTX, &ch, portMAX_DELAY) == pdTRUE)
            uart_putc_raw(HC06_UART_ID, ch);
    }
}

// ---------------------------------------------------------------------------
// task_bt_rx — processa bytes do HC-06: 'R'/'G'/'B' controlam LED RGB
// ---------------------------------------------------------------------------
static void task_bt_rx(void *p) {
    pwm_init_pin(LED_PIN_R);
    pwm_init_pin(LED_PIN_G);
    pwm_init_pin(LED_PIN_B);

    uint8_t ch;
    for (;;) {
        if (xQueueReceive(xQueueRX, &ch, portMAX_DELAY) == pdTRUE) {
            switch (ch) {
                case 'R':
                    pwm_set_gpio_level(LED_PIN_R, PWM_WRAP);
                    pwm_set_gpio_level(LED_PIN_G, 0);
                    pwm_set_gpio_level(LED_PIN_B, 0);
                    break;
                case 'G':
                    pwm_set_gpio_level(LED_PIN_R, 0);
                    pwm_set_gpio_level(LED_PIN_G, PWM_WRAP);
                    pwm_set_gpio_level(LED_PIN_B, 0);
                    break;
                case 'B':
                    pwm_set_gpio_level(LED_PIN_R, 0);
                    pwm_set_gpio_level(LED_PIN_G, 0);
                    pwm_set_gpio_level(LED_PIN_B, PWM_WRAP);
                    break;
                default:
                    break;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// task_mouse_x / task_mouse_y — lêem ADC e alimentam xQueueADC
// ---------------------------------------------------------------------------
static void task_mouse_x(void *p) {
    int center = *(int *)p;
    int buffer[MOVING_AVG_SIZE];
    for (int i = 0; i < MOVING_AVG_SIZE; i++) buffer[i] = center;
    int idx = 0;
    adc_t data = { .axis = 0 };

    for (;;) {
        xSemaphoreTake(xAdcMutex, portMAX_DELAY);
        int raw = read_adc_channel(ADC_INPUT_X);
        xSemaphoreGive(xAdcMutex);

        data.val = scale_adc(moving_average(buffer, raw, &idx, MOVING_AVG_SIZE),
                             center, MOUSE_SPEED_X, DEAD_ZONE_X);
        xQueueSend(xQueueADC, &data, 0);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void task_mouse_y(void *p) {
    int center = *(int *)p;
    int buffer[MOVING_AVG_SIZE];
    for (int i = 0; i < MOVING_AVG_SIZE; i++) buffer[i] = center;
    int idx = 0;
    adc_t data = { .axis = 1 };

    for (;;) {
        xSemaphoreTake(xAdcMutex, portMAX_DELAY);
        int raw = read_adc_channel(ADC_INPUT_Y);
        xSemaphoreGive(xAdcMutex);

        data.val = -scale_adc(moving_average(buffer, raw, &idx, MOVING_AVG_SIZE),
                              center, MOUSE_SPEED_Y, DEAD_ZONE_Y);
        xQueueSend(xQueueADC, &data, 0);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// ---------------------------------------------------------------------------
// task_adc_to_bt — converte adc_t em protocolo 0xFF|axis|lo|hi → xQueueTX
// ---------------------------------------------------------------------------
static void task_adc_to_bt(void *p) {
    adc_t data;
    for (;;) {
        if (xQueueReceive(xQueueADC, &data, portMAX_DELAY) == pdTRUE) {
            uint8_t pkt[4];
            pkt[0] = 0xFF;
            pkt[1] = (uint8_t)data.axis;
            pkt[2] = (uint8_t)(data.val & 0xFF);
            pkt[3] = (uint8_t)((data.val >> 8) & 0xFF);
            for (int i = 0; i < 4; i++)
                xQueueSend(xQueueTX, &pkt[i], 0);
        }
    }
}

// ---------------------------------------------------------------------------
// Hook de stack overflow
// ---------------------------------------------------------------------------
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName) {
    (void)xTask; (void)pcTaskName;
    for (;;);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(void) {
    stdio_init_all();

    // ADC
    adc_init();
    adc_gpio_init(VRX_PIN);
    adc_gpio_init(VRY_PIN);

    // Calibração do joystick (bloqueante, antes do RTOS)
    static int center_x, center_y;
    center_x = average_adc_channel(ADC_INPUT_X, CALIB_SAMPLES);
    center_y = average_adc_channel(ADC_INPUT_Y, CALIB_SAMPLES);

    // UART HC-06 + configuração inicial antes do scheduler (sleep_ms funciona aqui)
    init_uart_hc06();
    memcpy(current_pin, "1234", 5);
    hc06_config(HC06_NAME, "1234");
    init_uart_irq();

    // Queues
    xQueuePIN = xQueueCreate(1, sizeof(char[5]));
    xQueueRX  = xQueueCreate(256, sizeof(uint8_t));
    xQueueTX  = xQueueCreate(256, sizeof(uint8_t));
    xQueueADC = xQueueCreate(20,  sizeof(adc_t));

    xAdcMutex = xSemaphoreCreateMutex();

    // Tasks
    xTaskCreate(task_button,      "BTN",      512,  NULL,        2, NULL);
    xTaskCreate(task_hc06_config, "HC06CFG",  1024, NULL,        3, NULL);
    xTaskCreate(task_oled,        "OLED",     1024, NULL,        1, &xTaskOLED);
    xTaskCreate(task_led_status,  "LEDST",    512,  NULL,        1, NULL);
    xTaskCreate(task_bt_tx,       "BTTX",     512,  NULL,        3, NULL);
    xTaskCreate(task_bt_rx,       "BTRX",     512,  NULL,        2, NULL);
    xTaskCreate(task_mouse_x,     "MOUSEX",   1024, &center_x,   2, NULL);
    xTaskCreate(task_mouse_y,     "MOUSEY",   1024, &center_y,   2, NULL);
    xTaskCreate(task_adc_to_bt,   "ADC2BT",   512,  NULL,        2, NULL);

    vTaskStartScheduler();
    for (;;);
}
