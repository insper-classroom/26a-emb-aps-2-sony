# Tutorial — Como rodar o Skate Controller

## O que você vai precisar

- Raspberry Pi Pico com o circuito montado
- MPU6050 conectado via I2C
- Módulo HC-06 conectado via UART
- LED RGB e botões conectados
- Cabo micro-USB para flashar a Pico
- PC com Python instalado
- Jogo aberto no navegador (poki.com ou crazygames.com — Subway Surfers)

---

## Parte 1 — Hardware

### Conecte os componentes nos pinos corretos

| Componente | Pino da Pico |
|---|---|
| MPU6050 SDA | GP16 |
| MPU6050 SCL | GP17 |
| MPU6050 VCC | 3.3V |
| MPU6050 GND | GND |
| HC-06 TX | GP5 (RX da Pico) |
| HC-06 RX | GP4 (TX da Pico) |
| HC-06 VCC | 3.3V |
| HC-06 GND | GND |
| LED Vermelho | GP6 |
| LED Verde | GP2 |
| LED Azul | GP3 |
| Botão Start | GP18 |
| Botão Pause | GP19 |
| Botão Volume+ | GP20 |
| Botão Volume- | GP21 |

> Todos os botões usam pull-up interno — conecte um lado ao pino e o outro ao GND.

> O LED RGB deve ter resistores em série (220Ω a 330Ω em cada canal).

---

## Parte 2 — Compilar e flashar o firmware

### Pré-requisitos
- CMake instalado
- Toolchain ARM (`arm-none-eabi-gcc`)
- Pico SDK configurado
- `FREERTOS_KERNEL_PATH` apontando para a pasta `FreeRTOS-Kernel` do projeto

### Compilar

```bash
cd 26a-emb-aps-2-sony
mkdir -p build && cd build
cmake ..
make -j4
```

O arquivo gerado será `build/pico_emb.uf2`.

### Flashar na Pico

1. Segure o botão **BOOTSEL** da Pico
2. Conecte o cabo USB no PC enquanto segura o botão
3. Solte o botão — a Pico vai aparecer como um pen drive chamado `RPI-RP2`
4. Copie o arquivo `build/pico_emb.uf2` para dentro do pen drive
5. A Pico reinicia sozinha e o firmware já está rodando

---

## Parte 3 — Verificar se o firmware está funcionando

Com a Pico conectada via USB, abra um monitor serial (ex: `minicom`, `screen` ou o Serial Monitor do VS Code):

```bash
# Linux
screen /dev/ttyACM0 115200

# ou
minicom -D /dev/ttyACM0 -b 115200
```

Você deve ver no terminal:

```
=== Skate Controller — Dual-Core FreeRTOS ===
Core 0: sensor_ai | Core 1: bluetooth + led

[bt] UART1 OK — TX=GP4 RX=GP5 @ 9600 baud
[sensor] MPU6050 OK — iniciando amostragem a 83 Hz
```

Se aparecer `MPU6050 FALHOU`, verifique os fios do sensor.

---

## Parte 4 — Conectar o HC-06 via Bluetooth

### Linux

```bash
# 1. Abrir o bluetoothctl
bluetoothctl

# 2. Dentro do bluetoothctl:
scan on
# Aguarde aparecer HC-06 na lista
pair XX:XX:XX:XX:XX:XX    # use o endereço MAC do HC-06
# PIN padrão: 1234
trust XX:XX:XX:XX:XX:XX
exit

# 3. Criar a porta serial
sudo rfcomm bind 0 XX:XX:XX:XX:XX:XX
# Isso cria /dev/rfcomm0

# 4. Dar permissão (só precisa fazer uma vez)
sudo usermod -a -G dialout $USER
# Faça logout e login para aplicar
```

### Windows

1. Vá em **Configurações → Bluetooth → Adicionar dispositivo**
2. Procure `HC-06` e clique para parear
3. PIN: `1234`
4. Vá em **Mais configurações Bluetooth → Portas COM**
5. Anote a porta de saída (ex: `COM5`)

---

## Parte 5 — Instalar dependências do Python

```bash
pip install pyserial pynput
```

---

## Parte 6 — Rodar o controller Python

```bash
# Linux
python controller.py /dev/rfcomm0

# Windows
python controller.py COM5

# Detecta automaticamente (lista portas e você escolhe)
python controller.py
```

Você deve ver no terminal:

```
Conectando em /dev/rfcomm0 @ 9600 baud...
Conectado! Aguardando comandos do skate...

  J = pular (↑)     W = esquivar (←)
  S = start (Enter) P = pause (P)
  Ctrl+C para sair
```

---

## Parte 7 — Abrir o jogo e jogar

1. Abra o navegador e acesse **poki.com** ou **crazygames.com**
2. Procure por **Subway Surfers** e abra o jogo
3. **Clique UMA VEZ dentro da janela do jogo** para dar foco a ela
4. Não clique em mais nada — o script envia teclas para onde o foco estiver
5. Pressione o botão **Start (GP18)** no skate para iniciar a partida
6. Comece a se mover!

---

## Resumo dos movimentos

| Movimento no skate | LED acende | Tecla enviada | Ação no jogo |
|---|---|---|---|
| Parado | Apagado | nada | personagem corre |
| Empurrar para frente | Azul | ↑ | pular |
| Empurrar para trás | Roxo | ↓ | agachar |
| Inclinar esquerda | Verde | ← | mover esquerda |
| Inclinar direita | Amarelo | → | mover direita |
| Botão GP18 | — | Enter | start |
| Botão GP19 | — | P | pause |

---

## Problemas comuns

**MPU6050 FALHOU no terminal**
→ Verifique se SDA está em GP16 e SCL em GP17. Confirme que VCC é 3.3V (não 5V).

**LED não acende**
→ Verifique os resistores e se o LED é ânodo comum ou cátodo comum. O código usa active HIGH.

**Bluetooth não aparece para parear**
→ Certifique que o HC-06 está ligado (LED piscando). Se o LED estiver aceso fixo, já está pareado com outro dispositivo — desligue e religue.

**Teclas não aparecem no jogo**
→ Clique na janela do jogo no browser para garantir que ela tem o foco. O script envia teclas para a janela ativa.

**Personagem sempre vai para a mesma direção**
→ A ordem das classes do modelo pode estar errada. Abra `ei-model/model-parameters/model_variables.h`, procure `ei_classifier_inferencing_categories[]` e confirme que a ordem bate com os `LABEL_*` no topo do `main.cpp`.
