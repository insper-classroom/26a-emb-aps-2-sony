// =============================================================================
// TEST main.cpp — APS2 Sony — testa botões, LED RGB, MPU6050, HC-06
// =============================================================================
// Pinos:
//   Botões:  AZUL=18, AMARELO=19, VERDE=20, BRANCO=21 (pull-up, active-low)
//   LED RGB: R=4, G=2, B=3
//   MPU6050: SDA=16, SCL=17 (I2C0)
//   HC-06:   RXD=0, TXD=1 (UART0)
// =============================================================================

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "hardware/uart.h"
#include "hardware/pwm.h"
#include "FreeRTOS.h"
#include "task.h"

// ---------------------------------------------------------------------------
// Pinos
// ---------------------------------------------------------------------------
#define BTN_AZUL    19
#define BTN_AMARELO 20
#define BTN_VERDE   21
#define BTN_BRANCO  18

#define LED_R       6
#define LED_G       2
#define LED_B       3

#define I2C_SDA     16
#define I2C_SCL     17
#define I2C_PORT    i2c0

#define HC06_UART   uart1
#define HC06_RX     4
#define HC06_TX     5
#define HC06_BAUD   9600   // baud inicial do HC-06 de fábrica

// ---------------------------------------------------------------------------
// MPU6050
// ---------------------------------------------------------------------------
#define MPU_ADDR        0x68
#define MPU_REG_WHO_AM_I 0x75
#define MPU_REG_PWR_MGT  0x6B
#define MPU_REG_ACCEL_X  0x3B

static bool mpu_write_reg(uint8_t reg, uint8_t val) {
    uint8_t buf[2] = {reg, val};
    return i2c_write_blocking(I2C_PORT, MPU_ADDR, buf, 2, false) == 2;
}

static bool mpu_read_reg(uint8_t reg, uint8_t *buf, size_t len) {
    if (i2c_write_blocking(I2C_PORT, MPU_ADDR, &reg, 1, true) != 1)
        return false;
    return i2c_read_blocking(I2C_PORT, MPU_ADDR, buf, len, false) == (int)len;
}

// ---------------------------------------------------------------------------
// PWM helper — brilho 0-100
// ---------------------------------------------------------------------------
#define PWM_WRAP 1000

static void pwm_setup(uint gpio) {
    gpio_set_function(gpio, GPIO_FUNC_PWM);
    uint slice = pwm_gpio_to_slice_num(gpio);
    pwm_set_wrap(slice, PWM_WRAP);
    pwm_set_gpio_level(gpio, 0);
    pwm_set_enabled(slice, true);
}

static void rgb_set(uint r, uint g, uint b) {
    // LED RGB ânodo comum → nível alto = apagado, baixo = aceso
    // Se o seu LED for cátodo comum, inverta: use o valor direto
    // Ajuste aqui se as cores estiverem invertidas!
    pwm_set_gpio_level(LED_R, r);
    pwm_set_gpio_level(LED_G, g);
    pwm_set_gpio_level(LED_B, b);
}

// ---------------------------------------------------------------------------
// Task: testa botões — imprime qual botão foi pressionado
// ---------------------------------------------------------------------------
static void task_test_buttons(void *p) {
    const uint btns[] = {BTN_AZUL, BTN_AMARELO, BTN_VERDE, BTN_BRANCO};
    const char *names[] = {"AZUL", "AMARELO", "VERDE", "BRANCO"};

    for (int i = 0; i < 4; i++) {
        gpio_init(btns[i]);
        gpio_set_dir(btns[i], GPIO_IN);
        gpio_pull_up(btns[i]);
    }

    bool last[4] = {true, true, true, true};

    printf("[BTN] Task iniciada. Pressione os botoes.\n");

    for (;;) {
        for (int i = 0; i < 4; i++) {
            bool cur = gpio_get(btns[i]);
            if (last[i] && !cur) {
                // borda de descida = pressionado
                vTaskDelay(pdMS_TO_TICKS(30)); // debounce
                if (!gpio_get(btns[i])) {
                    printf("[BTN] Botao %s (GPIO %u) PRESSIONADO\n", names[i], btns[i]);
                }
            }
            last[i] = cur;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ---------------------------------------------------------------------------
// Task: testa LED RGB — cicla R → G → B → branco → apaga
// ---------------------------------------------------------------------------
static void task_test_rgb(void *p) {
    pwm_setup(LED_R);
    pwm_setup(LED_G);
    pwm_setup(LED_B);

    const char *cores[] = {"VERMELHO", "VERDE", "AZUL", "BRANCO", "APAGADO"};
    // {R, G, B} em % do PWM_WRAP
    const uint vals[5][3] = {
        {PWM_WRAP, 0,        0       },
        {0,        PWM_WRAP, 0       },
        {0,        0,        PWM_WRAP},
        {PWM_WRAP, PWM_WRAP, PWM_WRAP},
        {0,        0,        0       },
    };

    printf("[RGB] Task iniciada. Ciclando cores.\n");
    int idx = 0;

    for (;;) {
        printf("[RGB] Cor: %s\n", cores[idx]);
        rgb_set(vals[idx][0], vals[idx][1], vals[idx][2]);
        idx = (idx + 1) % 5;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// ---------------------------------------------------------------------------
// Task: testa MPU6050 — lê WHO_AM_I e depois aceleração em loop
// ---------------------------------------------------------------------------
static void task_test_mpu(void *p) {
    // I2C já inicializado no main antes do scheduler

    printf("[MPU] Verificando WHO_AM_I...\n");

    uint8_t who = 0;
    if (!mpu_read_reg(MPU_REG_WHO_AM_I, &who, 1)) {
        printf("[MPU] ERRO: nao conseguiu ler I2C. Verifique SDA/SCL e endereco.\n");
        vTaskDelete(NULL);
        return;
    }

    // MPU6050 retorna 0x68, MPU6500 retorna 0x70
    printf("[MPU] WHO_AM_I = 0x%02X %s\n", who,
           (who == 0x68 || who == 0x70) ? "(OK)" : "(INESPERADO — verifique sensor)");

    // Acorda o MPU (sai do sleep)
    mpu_write_reg(MPU_REG_PWR_MGT, 0x00);
    vTaskDelay(pdMS_TO_TICKS(100));

    printf("[MPU] Lendo aceleracao...\n");

    for (;;) {
        uint8_t raw[6];
        if (mpu_read_reg(MPU_REG_ACCEL_X, raw, 6)) {
            int16_t ax = (int16_t)((raw[0] << 8) | raw[1]);
            int16_t ay = (int16_t)((raw[2] << 8) | raw[3]);
            int16_t az = (int16_t)((raw[4] << 8) | raw[5]);
            // Converte para g (escala padrão ±2g → divisor 16384)
            float fax = ax / 16384.0f;
            float fay = ay / 16384.0f;
            float faz = az / 16384.0f;
            printf("[MPU] AX=%.3f  AY=%.3f  AZ=%.3f (g)\n", fax, fay, faz);
        } else {
            printf("[MPU] ERRO na leitura\n");
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

// ---------------------------------------------------------------------------
// Task: testa HC-06 — tenta AT em 9600 e 115200, depois fica em echo
// ---------------------------------------------------------------------------
static bool hc06_try_at(uint baud) {
    uart_set_baudrate(HC06_UART, baud);
    vTaskDelay(pdMS_TO_TICKS(200));
    // limpa rx
    while (uart_is_readable(HC06_UART)) uart_getc(HC06_UART);

    uart_puts(HC06_UART, "AT");
    vTaskDelay(pdMS_TO_TICKS(600));

    char resp[32];
    int i = 0;
    while (uart_is_readable(HC06_UART) && i < 31)
        resp[i++] = uart_getc(HC06_UART);
    resp[i] = '\0';

    if (i > 0)
        printf("[BT] Baud %u -> '%s'\n", baud, resp);

    return strstr(resp, "OK") != NULL;
}

static void task_test_hc06(void *p) {
    printf("[BT] Testando HC-06...\n");

    bool ok = false;
    uint bauds[] = {9600, 115200, 38400};
    for (int b = 0; b < 3 && !ok; b++) {
        printf("[BT] Tentando %u baud\n", bauds[b]);
        ok = hc06_try_at(bauds[b]);
    }

    if (!ok) {
        printf("[BT] ERRO: HC-06 nao respondeu em nenhum baud.\n");
        printf("     Verifique cabos: GPIO4(RX Pico)->TXD HC06, GPIO5(TX Pico)->RXD HC06\n");
        vTaskDelete(NULL);
        return;
    }

    printf("[BT] HC-06 OK! Modo echo ativo.\n");
    for (;;) {
        if (uart_is_readable(HC06_UART)) {
            char c = uart_getc(HC06_UART);
            printf("[BT] Recebeu: '%c' (0x%02X)\n", c, (uint8_t)c);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ---------------------------------------------------------------------------
// Stack overflow hook
// ---------------------------------------------------------------------------
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName) {
    (void)xTask; (void)pcTaskName;
    printf("STACK OVERFLOW em task: %s\n", pcTaskName);
    for (;;);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(void) {
    stdio_init_all();
    sleep_ms(2000); // aguarda USB/serial conectar

    printf("\n========================================\n");
    printf("  APS2 — Teste de Hardware\n");
    printf("========================================\n");

    // I2C0 para MPU6050
    i2c_init(I2C_PORT, 400000);
    gpio_set_function(I2C_SDA, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA);
    gpio_pull_up(I2C_SCL);
    printf("[INIT] I2C0 iniciado (SDA=GPIO%d, SCL=GPIO%d)\n", I2C_SDA, I2C_SCL);

    // UART0 para HC-06
    uart_init(HC06_UART, HC06_BAUD);
    gpio_set_function(HC06_RX, GPIO_FUNC_UART);
    gpio_set_function(HC06_TX, GPIO_FUNC_UART);
    uart_set_hw_flow(HC06_UART, false, false);
    uart_set_format(HC06_UART, 8, 1, UART_PARITY_NONE);
    printf("[INIT] UART0 iniciado (RX=GPIO%d, TX=GPIO%d) @ %d baud\n",
           HC06_RX, HC06_TX, HC06_BAUD);

    // Tasks de teste
    xTaskCreate(task_test_buttons, "BTN",  1024, NULL, 2, NULL);
    xTaskCreate(task_test_rgb,     "RGB",  1024, NULL, 1, NULL);
    xTaskCreate(task_test_mpu,     "MPU",  2048, NULL, 2, NULL);
    xTaskCreate(task_test_hc06,    "BT",   2048, NULL, 1, NULL);

    vTaskStartScheduler();
    for (;;);
}