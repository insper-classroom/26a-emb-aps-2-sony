#ifndef HC06_H_
#define HC06_H_

#include <stdbool.h>
#include "pico/stdlib.h"
#include "hardware/uart.h"

/* ── Pinos e periférico ───────────────────────────────────────────────────── */
#define HC06_UART_ID    uart1
#define HC06_BAUD_RATE  115200   /* baud final após configuração AT          */

/* Pinos do módulo HC-06 (nomes da perspectiva do HC-06) */
#define HC06_TX_PIN     5        /* HC-06 TX → Pico UART1_RX  (GP5)         */
#define HC06_RX_PIN     4        /* HC-06 RX ← Pico UART1_TX  (GP4)         */

/* Pino KEY/EN do HC-06 — usado apenas durante configuração AT               */
/* Conecte o pino KEY do módulo ao GP10 (ou deixe solto se não tiver)        */
#define HC06_ENABLE_PIN 10

/* Pino STATE do HC-06 — HIGH quando pareado e conectado                     */
#define HC06_STATE_PIN  22

/* ── API pública ─────────────────────────────────────────────────────────── */
bool hc06_check_connection(void);
bool hc06_set_name(char name[]);
bool hc06_set_pin(char pin[]);
bool hc06_set_baud_115200(void);
bool hc06_set_at_mode(int on);
bool hc06_config(char name[], char pin[]);

#endif // HC06_H_
