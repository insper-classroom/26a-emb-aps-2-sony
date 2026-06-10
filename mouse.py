"""
Mouse wireless via Bluetooth HC-06
===================================
Protocolo Pico → PC (movimento):
    4 bytes: 0xFF | axis (0=X, 1=Y) | val_lo | val_hi
    val é signed 16-bit (little-endian)

Protocolo PC → Pico (cor RGB):
    1 byte: 'R', 'G' ou 'B'

Dependências:
    pip install pyserial pyautogui
"""

import struct
import threading
import serial
import serial.tools.list_ports
import pyautogui

pyautogui.FAILSAFE = False
pyautogui.PAUSE = 0


def selecionar_porta():
    portas = serial.tools.list_ports.comports()

    print("\nPortas disponíveis:")
    for i, p in enumerate(portas):
        print(f"  [{i}] {p.device} — {p.description}")

    print("\nDigite o número da porta ou o caminho direto (ex: /dev/rfcomm0):")
    entrada = input("> ").strip()

    if entrada.startswith("/"):
        return entrada

    try:
        idx = int(entrada)
        if 0 <= idx < len(portas):
            return portas[idx].device
    except ValueError:
        pass

    return entrada


def thread_rgb(ser):
    """Lê teclas do terminal e envia comando de cor para a Pico."""
    print("\nComandos de cor: pressione R, G ou B + Enter")
    while True:
        try:
            tecla = input().strip().upper()
            if tecla in ('R', 'G', 'B'):
                ser.write(tecla.encode())
                print(f"  → cor enviada: {tecla}")
            else:
                print("  Tecla inválida. Use R, G ou B.")
        except (EOFError, KeyboardInterrupt):
            break


def main():
    porta = selecionar_porta()
    print(f"\nConectando em {porta} @ 115200 baud...")

    with serial.Serial(porta, 115200, timeout=1) as ser:
        print("Conectado! Movimente o joystick para controlar o mouse.\n")

        # Thread separada para envio de cores RGB
        t = threading.Thread(target=thread_rgb, args=(ser,), daemon=True)
        t.start()

        buf = bytearray()

        while True:
            data = ser.read(64)
            if not data:
                continue
            buf.extend(data)

            # Processa todos os pacotes completos (4 bytes cada)
            while len(buf) >= 4:
                # Busca marcador de início 0xFF
                if buf[0] != 0xFF:
                    buf.pop(0)
                    continue

                pkt = buf[:4]
                buf = buf[4:]

                axis  = pkt[1]
                val   = struct.unpack('<h', bytes([pkt[2], pkt[3]]))[0]  # signed 16-bit LE

                if axis == 0:
                    pyautogui.moveRel(val, 0)
                elif axis == 1:
                    pyautogui.moveRel(0, val)


if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        print("\nEncerrando.")
