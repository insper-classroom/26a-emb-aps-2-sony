# Skate Controller para Subway Surfers — Documentação do Projeto

## Visão Geral

Controle físico imersivo para o jogo *Subway Surfers* utilizando um skate real como interface.
O skate possui um **sensor MPU6050 (IMU)** que captura os movimentos e os classifica em tempo real
com um modelo de **Machine Learning (TinyML)** rodando diretamente no microcontrolador (**Raspberry Pi Pico**).
Os comandos são enviados via **Bluetooth (HC-06)** para o PC, onde um script Python pressiona as teclas do jogo.

---

## Hardware

| Componente | Pino(s) |
|---|---|
| MPU6050 (IMU) — SDA | GP0 (I2C0) |
| MPU6050 (IMU) — SCL | GP1 (I2C0) |
| HC-06 Bluetooth — TX da Pico | GP12 (UART0) |
| HC-06 Bluetooth — RX da Pico | GP13 (UART0) |
| Botão Start | GP4 |
| Botão Pause | GP5 |
| Botão Volume | GP6 |
| LED RGB — Vermelho | GP7 |
| LED RGB — Verde | GP8 |
| LED RGB — Azul | GP9 |
| Debug WCET — Sensor task | GP20 |
| Debug WCET — BT task | GP21 |
| Debug WCET — LED task | GP22 |

> Os pinos GP20, GP21 e GP22 são para medir WCET e Jitter com osciloscópio (lab de RTOS/SMP).

---

## Arquitetura do Firmware (FreeRTOS)

### Tasks

| Task | Prioridade | Stack | Função |
|---|---|---|---|
| `task_sensor_ai` | 3 (alta) | 16 KB | Lê MPU6050 a 83 Hz, roda inferência EI, envia resultado |
| `task_bluetooth` | 2 | 2 KB | Recebe classificações e botões, envia comandos via UART |
| `task_led` | 1 (baixa) | 1 KB | Atualiza LED RGB conforme movimento detectado |

### Filas (Queues)

| Fila | De → Para | Conteúdo |
|---|---|---|
| `xQueueMotion` | `task_sensor_ai` → `task_bluetooth` | `int` (label do movimento) |
| `xQueueButtons` | ISR → `task_bluetooth` | `int` (ID do botão) |
| `xQueueLed` | `task_sensor_ai` → `task_led` | `int` (label do movimento) |

### ISR

- `gpio_irq_handler` — acionada nos botões (GP4, GP5, GP6), envia para `xQueueButtons` via `xQueueSendFromISR`

---

## Modelo de Machine Learning (Edge Impulse)

### Classes treinadas

| Classe | Movimento físico no skate | Comando enviado | Tecla no jogo |
|---|---|---|---|
| `backward` | Empurrar para trás (tail down) | `C` | ↓ (agachar) |
| `forward` | Empurrar para frente (nose down) | `J` | ↑ (pular) |
| `idle` | Parado / rolando reto | `I` | nada |
| `left` | Inclinar para esquerda | `L` | ← |
| `right` | Inclinar para direita | `R` | → |

> **Atenção:** O Edge Impulse ordena as classes alfabeticamente. Após exportar o modelo, confirme a ordem em
> `ei-model/model-parameters/model_variables.h` → `ei_classifier_inferencing_categories[]`
> e ajuste os `LABEL_*` no `main.cpp` se necessário.

### Configurações do Impulse (Edge Impulse)

| Parâmetro | Valor |
|---|---|
| Sensor | Acelerômetro (accX, accY, accZ) |
| Frequência | 83 Hz |
| Window size | 2000 ms |
| Window increase | 200 ms |
| Processing block | Spectral Analysis |
| Learning block | Classification (Keras) |

### LED RGB — feedback visual

| Cor | Movimento |
|---|---|
| Apagado | idle |
| Azul | forward (pular) |
| Roxo | backward (agachar) |
| Verde | left |
| Amarelo | right |

---

## Processo de Treinamento (passo a passo)

### Parte 1 — Coletar dados

1. Instalar o CLI do Edge Impulse:
   ```bash
   npm install -g edge-impulse-cli
   ```

2. Flashar o firmware de coleta (`edgeimpulse-dataforwarding`) na Pico com o MPU6050 conectado.

3. Rodar o data forwarder:
   ```bash
   edge-impulse-data-forwarder
   ```

4. No Edge Impulse, gravar ~2 minutos de cada classe: `backward`, `forward`, `idle`, `left`, `right`.

### Parte 2 — Treinar

5. Criar o Impulse com os parâmetros acima.
6. Adicionar **Spectral Analysis** como processing block.
7. Adicionar **Classification (Keras)** como learning block.
8. Treinar (mínimo 30 epochs).

### Parte 3 — Exportar e integrar

9. **Deployment → C++ Library → Build** → baixa um `.zip`.

10. Substituir o conteúdo de `ei-model/` com os arquivos do `.zip`:
    ```
    ei-model/
      edge-impulse-sdk/   ← nova versão
      model-parameters/   ← novos parâmetros/labels
      tflite-model/       ← novo modelo compilado
    ```

11. Recompilar o firmware — nenhuma mudança de código necessária.

---

## CMakeLists.txt — Integração do EI SDK

O `main/CMakeLists.txt` foi atualizado para:
- Adicionar `${MODEL_FOLDER}` (`ei-model/`) como include directory → resolve `"edge-impulse-sdk/classifier/..."`.
- Compilar as fontes do EI SDK: compiled model, DSP, TFLite Micro, CMSIS-DSP.
- Adicionar defines necessários: `EI_CLASSIFIER_COMPILED=1`, `EIDSP_USE_CMSIS_DSP=1`, etc.

---

## Script Python — `controller.py`

Recebe comandos via Bluetooth serial e pressiona teclas no jogo.

### Instalação

```bash
pip install pyserial pynput
```

### Uso

```bash
# Linux (HC-06 via rfcomm)
python controller.py /dev/rfcomm0

# Windows
python controller.py COM5

# Detecta porta automaticamente
python controller.py
```

### Mapeamento de teclas

| Comando recebido | Tecla | Ação no jogo |
|---|---|---|
| `J` | ↑ | Pular |
| `L` | ← | Mover esquerda |
| `R` | → | Mover direita |
| `C` | ↓ | Agachar/rolar |
| `S` | Enter | Start |
| `P` | P | Pause |

---

## Conectar HC-06 no Linux

```bash
# 1. Parear
bluetoothctl
> scan on
> pair XX:XX:XX:XX:XX:XX
> trust XX:XX:XX:XX:XX:XX
> exit

# 2. Criar porta serial
sudo rfcomm bind 0 XX:XX:XX:XX:XX:XX

# 3. Permissão (fazer uma vez, exige logout/login)
sudo usermod -a -G dialout $USER

# 4. Rodar
python controller.py /dev/rfcomm0
```

---

## Onde jogar (Linux)

**Opção 1 — Navegador (mais simples):**
Acesse **poki.com** ou **crazygames.com** e procure "Subway Surfers".
O jogo responde às teclas de seta que o script envia.

**Opção 2 — Waydroid (Android no Linux):**
```bash
sudo apt install waydroid
sudo waydroid init
sudo systemctl start waydroid-container
waydroid show-full-ui
```
Instale o Subway Surfers pela Play Store dentro do Waydroid.

> **Importante:** Após rodar o `controller.py`, clique UMA VEZ na janela do jogo
> para dar foco a ela. O script envia teclas para a janela em foco.

---

## RTOS — Lab SMP (para o lab do Insper)

O `FreeRTOSConfig.h` já está preparado para SMP (2 núcleos do RP2040/RP2350).
Para ativar, no `CMakeLists.txt`:

1. Trocar `configNUMBER_OF_CORES=1` por `configNUMBER_OF_CORES=2`
2. Adicionar `FREE_RTOS_KERNEL_SMP=1` nos compile definitions

Para distribuir as tasks entre os núcleos:
```cpp
vTaskCoreAffinitySet(h_sensor, (1 << 0)); // Core 0: sensor + IA
vTaskCoreAffinitySet(h_bt,     (1 << 1)); // Core 1: bluetooth
vTaskCoreAffinitySet(h_led,    (1 << 1)); // Core 1: led
```

Os pinos GP20/21/22 (debug) permitem medir **WCET** e **Jitter** de cada task com osciloscópio.

---

## Estrutura de arquivos relevantes

```
26a-emb-aps-2-sony/
├── main/
│   ├── main.cpp          ← firmware completo (RTOS + MPU6050 + EI + BT)
│   ├── CMakeLists.txt    ← build com EI SDK integrado
│   ├── pins.h            ← definição de pinos
│   ├── mpu6050.h         ← registradores do MPU6050
│   └── FreeRTOSConfig.h  ← configuração FreeRTOS (SMP pronto)
├── ei-model/             ← modelo Edge Impulse exportado
│   ├── edge-impulse-sdk/
│   ├── model-parameters/
│   └── tflite-model/
├── controller.py         ← script Python (Bluetooth → teclas do jogo)
└── FreeRTOS-Kernel/      ← kernel FreeRTOS
```
