/*
 * Skate Controller — Subway Surfers
 * FreeRTOS + MPU6050 (inclinação direta) + HC-06 Bluetooth
 *
 *   MPU6050 : I2C0  SDA=GP16, SCL=GP17
 *   LED RGB : GP6 (R), GP3 (G), GP2 (B)
 *   HC-06   : UART1 TX=GP4, RX=GP5
 *
 * Comandos enviados:
 *   'L' = esquerda  'R' = direita  'J' = pulo  'C' = abaixar  'I' = parado
 */

#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "hardware/gpio.h"
#include "FreeRTOS.h"
#include "task.h"
#include "hc06.h"
#include "mpu6050.h"
#include "pins.h"

#define MPU_I2C  i2c0
#define MPU_SDA  16
#define MPU_SCL  17
#define MPU_ADDR MPU6050_I2C_DEFAULT
#define MPU_SENS 16384.0f            /* LSB/g @ ±2g */

/* Ajuste o limiar conforme a sensibilidade desejada (em m/s²) */
#define TILT_THRESHOLD 3.0f

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
    i2c_read_blocking(MPU_I2C, MPU_ADDR, &id, 1, false);
    if (id != 0x68) return false;

    mpu_write(MPUREG_ACCEL_CONFIG, 0x00);
    return true;
}

static void mpu_read(float *ax, float *ay, float *az) {
    uint8_t reg = MPUREG_ACCEL_XOUT_H, raw[6];
    i2c_write_blocking(MPU_I2C, MPU_ADDR, &reg, 1, true);
    i2c_read_blocking(MPU_I2C, MPU_ADDR, raw, 6, false);
    *ax = (int16_t)((raw[0] << 8) | raw[1]) / MPU_SENS * 9.80665f;
    *ay = (int16_t)((raw[2] << 8) | raw[3]) / MPU_SENS * 9.80665f;
    *az = (int16_t)((raw[4] << 8) | raw[5]) / MPU_SENS * 9.80665f;
}

static void set_led(bool r, bool g, bool b) {
    gpio_put(LED_PIN_R, r);
    gpio_put(LED_PIN_G, g);
    gpio_put(LED_PIN_B, b);
}

static void task_skate(void *p) {
    (void)p;

    gpio_init(LED_PIN_R); gpio_set_dir(LED_PIN_R, GPIO_OUT); gpio_put(LED_PIN_R, 0);
    gpio_init(LED_PIN_G); gpio_set_dir(LED_PIN_G, GPIO_OUT); gpio_put(LED_PIN_G, 0);
    gpio_init(LED_PIN_B); gpio_set_dir(LED_PIN_B, GPIO_OUT); gpio_put(LED_PIN_B, 0);

    if (!mpu_init()) {
        printf("[mpu] nao encontrado!\n");
        set_led(1, 0, 0);
        vTaskDelete(NULL);
        return;
    }
    printf("[mpu] OK\n");

    char last_cmd = 'I';

    while (1) {
        float ax, ay, az;
        mpu_read(&ax, &ay, &az);

        /* Determina o eixo de maior inclinação */
        float abs_ax = ax < 0 ? -ax : ax;
        float abs_ay = ay < 0 ? -ay : ay;

        char cmd = 'I';
        if (abs_ax > abs_ay && abs_ax > TILT_THRESHOLD) {
            cmd = (ax < 0) ? 'L' : 'R';
        } else if (abs_ay > TILT_THRESHOLD) {
            cmd = (ay > 0) ? 'J' : 'C';
        }

        if (cmd != last_cmd) {
            last_cmd = cmd;
            uart_putc(HC06_UART_ID, cmd);
            printf("Cmd: %c  ax=%.2f  ay=%.2f\n", cmd, ax, ay);

            switch (cmd) {
                case 'L': set_led(0, 1, 0); break;  /* verde   */
                case 'R': set_led(1, 1, 0); break;  /* amarelo */
                case 'J': set_led(0, 0, 1); break;  /* azul    */
                case 'C': set_led(1, 0, 1); break;  /* roxo    */
                default:  set_led(0, 0, 0); break;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

int main(void) {
    stdio_init_all();

    uart_init(HC06_UART_ID, HC06_BAUD_RATE);
    gpio_set_function(HC06_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(HC06_RX_PIN, GPIO_FUNC_UART);

    xTaskCreate(task_skate, "skate", 2048, NULL, 2, NULL);
    vTaskStartScheduler();

    while (1) {}
}
