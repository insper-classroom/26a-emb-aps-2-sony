#include <stdio.h>

#include "pico/stdlib.h"
#include "pico/stdio_usb.h"

int main()
{
    stdio_init_all();

    printf("Boot do firmware iniciado.\n");
    printf("Aguardando monitor USB...\n");

    // Se o monitor abrir depois, ainda continuamos imprimindo.
    for (int i = 0; i < 50 && !stdio_usb_connected(); i++) {
        sleep_ms(100);
    }

    printf("USB conectado? %s\n", stdio_usb_connected() ? "sim" : "nao");
    printf("Hello world simples em C++!\n");

    while (true) {
        printf("Rodando...\n");
        sleep_ms(1000);
    }

    return 0;
}