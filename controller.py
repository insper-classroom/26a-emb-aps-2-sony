#!/usr/bin/env python3
"""
Subway Surfers - Skate Controller
Recebe comandos do HC-06 via Bluetooth serial e pressiona teclas no jogo.

Instalar dependências:
    pip install pyserial pynput

Uso:
    python controller.py              # lista portas e deixa você escolher
    python controller.py COM5         # Windows: porta específica
    python controller.py /dev/rfcomm0 # Linux: porta específica
"""

import sys
import time
import serial
import serial.tools.list_ports
from pynput.keyboard import Key, Controller

# ── Configuração ──────────────────────────────────────────────────────────────

BAUD_RATE = 115200

# Comando recebido → tecla pressionada
# Mude KEY_WAVE para Key.right se quiser esquivar para a direita
KEY_WAVE = Key.left

COMMANDS = {
    b'J': Key.up,      # jump   → pular       (↑  — padrão BlueStacks)
    b'L': Key.left,    # left   → esquerda    (←)
    b'R': Key.right,   # right  → direita     (→)
    b'C': Key.down,    # crouch → abaixar     (↓)
    b'I': None,        # idle   → nada
    b'S': Key.space,   # start      → Espaço
    b'P': Key.esc,     # pause      → ESC
    b'U': None,        # volume up  → sem mapeamento no jogo
    b'D': None,        # volume down → sem mapeamento no jogo
}

# Cooldown mínimo entre o mesmo comando (ms) — evita spam de tecla
COOLDOWN = {
    b'J': 600,
    b'L': 400,
    b'R': 400,
    b'C': 400,
    b'S': 1500,
    b'P': 1000,
    b'U': 500,
    b'D': 500,
}
DEFAULT_COOLDOWN = 300

PRESS_DURATION = 0.08   # segundos que a tecla fica pressionada

# ── Utilitários ───────────────────────────────────────────────────────────────

keyboard = Controller()

def press_key(key):
    """Pressiona e solta uma tecla."""
    if key is None:
        return
    keyboard.press(key)
    time.sleep(PRESS_DURATION)
    keyboard.release(key)


def list_ports():
    """Imprime todas as portas seriais disponíveis."""
    ports = list(serial.tools.list_ports.comports())
    if not ports:
        print("Nenhuma porta serial encontrada.")
        return []
    print("\nPortas disponíveis:")
    for i, p in enumerate(ports):
        print(f"  [{i}] {p.device:12s} — {p.description}")
    return ports


def choose_port():
    """Lista portas e pede ao usuário para escolher."""
    ports = list_ports()
    if not ports:
        sys.exit(1)
    try:
        idx = int(input("\nEscolha o número da porta do HC-06: "))
        return ports[idx].device
    except (ValueError, IndexError):
        print("Escolha inválida.")
        sys.exit(1)


# ── Loop principal ────────────────────────────────────────────────────────────

def run(port: str):
    last_sent: dict[bytes, float] = {}

    print(f"\nConectando em {port} @ {BAUD_RATE} baud...")
    try:
        ser = serial.Serial(port, BAUD_RATE, timeout=1)
    except serial.SerialException as e:
        print(f"Erro ao abrir porta: {e}")
        sys.exit(1)

    print("Conectado! Aguardando comandos do skate...\n")
    print("  J = pular (↑)        C = agachar (↓)")
    print("  L = esquerda (←)    R = direita (→)")
    print("  S = start (Espaço)  P = pause (ESC)")
    print("  Ctrl+C para sair\n")

    buffer = b""

    try:
        while True:
            # Lê bytes disponíveis
            try:
                chunk = ser.read(ser.in_waiting or 1)
            except serial.SerialException:
                print("Conexão perdida. Reconectando em 3s...")
                ser.close()
                time.sleep(3)
                try:
                    ser = serial.Serial(port, BAUD_RATE, timeout=1)
                    print("Reconectado.")
                except serial.SerialException:
                    print("Falhou. Tentando novamente...")
                continue

            buffer += chunk

            # Processa cada linha terminada em \n
            while b'\n' in buffer:
                line, buffer = buffer.split(b'\n', 1)
                cmd = line.strip()
                if not cmd:
                    continue

                now = time.time() * 1000  # ms
                cooldown = COOLDOWN.get(cmd, DEFAULT_COOLDOWN)
                last = last_sent.get(cmd, 0)

                if now - last < cooldown:
                    continue  # ainda em cooldown

                key = COMMANDS.get(cmd)
                if key is not None:
                    last_sent[cmd] = now
                    label = {
                        b'J': "PULAR    ↑",
                        b'L': "ESQUERDA ←",
                        b'R': "DIREITA  →",
                        b'C': "AGACHAR  ↓",
                        b'S': "START    ⎵",
                        b'P': "PAUSE    ESC",
                        b'U': "VOL UP    ",
                        b'D': "VOL DOWN  ",
                    }.get(cmd, cmd.decode(errors='?'))
                    print(f"[{time.strftime('%H:%M:%S')}] {label}")
                    press_key(key)
                elif cmd == b'I':
                    pass  # idle, silencioso
                else:
                    print(f"[{time.strftime('%H:%M:%S')}] Comando desconhecido: {cmd}")

    except KeyboardInterrupt:
        print("\nEncerrado pelo usuário.")
    finally:
        ser.close()


# ── Entry point ───────────────────────────────────────────────────────────────

if __name__ == "__main__":
    if len(sys.argv) == 2:
        run(sys.argv[1])
    else:
        port = choose_port()
        run(port)
