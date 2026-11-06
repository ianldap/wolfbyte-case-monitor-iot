# Monitor IoT de Conforto Acústico e Acessibilidade 
**Projeto desenvolvido para a fase Trainee da equipe WolfByte (Hardware) do Ramo Estudantil IEEE CEFET/RJ (2026.2)**

Este projeto consiste em um dispositivo IoT embarcado projetado para acompanhar o nível de ruído de um ambiente em tempo real. O objetivo principal é auxiliar na identificação de ambientes potencialmente desconfortáveis para pessoas com hipersensibilidade auditiva, promovendo acessibilidade e conforto.

---

## Hardware Utilizado
* **Microcontrolador:** DOIT ESP32 DevKit V1
* **Sensor Acústico:** Microfone Analógico MAX4466 (conectado ao pino ADC 34)
* **Interface Visual Local:** Display OLED SSD1306 (Comunicação I2C)
* **Interface Visual Remota:** Computador/Smartphone rodando MQTT Explorer

 Instruções de Montagem do Circuito
As conexões físicas entre os componentes e o ESP32 devem seguir o esquema abaixo. 
###<img width="612" height="690" alt="circuito-monitor-acustico-iot" src="https://github.com/user-attachments/assets/dc514e6f-3d1a-42a2-9bdf-0e7744993834" />

**Microfone MAX4466:**
* PINO **VCC** -> Conectado ao pino **3V3** do ESP32.
* PINO **GND** -> Conectado ao pino **GND** do ESP32.
* PINO **OUT** -> Conectado ao pino **D34** (ADC1) do ESP32.

**Display OLED SSD1306 (I2C):**
* PINO **VCC** -> Conectado ao pino **3V3** do ESP32.
* PINO **GND** -> Conectado ao pino **GND** do ESP32.
* PINO **SDA** -> Conectado ao pino **D21** do ESP32.
* PINO **SCL** -> Conectado ao pino **D22** do ESP32.

*Atenção:* Certifique-se de alimentar os módulos com 3.3V, pois o pino ADC do ESP32 suporta no máximo essa tensão.

---

## Arquitetura de Comunicação
A captura de som ocorre fisicamente através do sensor **MAX4466**. O sinal elétrico (tensão) é lido pelo conversor Analógico-Digital (ADC) do **ESP32**. 
Após o processamento matemático local para extrair o valor RMS, o ESP32 utiliza a rede Wi-Fi local para publicar os dados periodicamente (a cada 2 segundos) utilizando o protocolo leve **MQTT**. 
Os dados são enviados ao broker público `broker.hivemq.com` na porta `1883`, sob os tópicos `ieee/cefet/ruido/rms` e `ieee/cefet/ruido/estado`. Uma interface rodando o cliente MQTT assina esses tópicos e exibe o histórico acústico do ambiente em tempo real, independente da distância física do sensor.

---

## Especificações Técnicas e Processamento de Sinal

### 1. Taxa de Amostragem
O microfone é amostrado a uma taxa de **8000 Hz** (8 mil leituras por segundo). Uma janela de tempo de 1 segundo é utilizada para agrupar as amostras. Essa alta frequência garante que picos sonoros rápidos (como palmas ou batidas) sejam capturados sem perda de informação (aliasing).

### 2. Método de Filtragem (Cálculo RMS)
Sensores acústicos analógicos como o MAX4466 operam com um "DC Offset" (uma tensão de repouso contínua, geralmente metade da tensão de alimentação). O simples cálculo de média aritmética falharia ao tentar medir o ruído.
Para resolver isso, o sistema aplica um filtro matemático de **Variância e Raiz Quadrada (Root Mean Square - RMS)**. O software eleva as leituras ao quadrado e subtrai o DC offset, resultando na energia acústica real e puramente variável do ambiente (RMS relativo).

### 3. Classificação, Limites e Histerese
Os limites foram calibrados empiricamente em ambiente real de testes. 
* O ruído basal do ambiente (com ar condicionado ligado) marcou um RMS relativo constante na faixa de `110`. 
* **Limites adotados:** Transição de Calmo para Moderado em `160 RMS`, e de Moderado para Ruidoso em `300 RMS`.
* **Histerese:** Foi aplicado um fator de histerese de **80%** no limite inferior. Caso o ambiente atinja o estado "Moderado" (>160), ele só retornará para "Calmo" quando o nível sonoro cair abaixo de `128` (160 * 0.80). Essa margem de 20% evita alternâncias rápidas e piscadas constantes na interface quando o ruído oscila próximo à linha de corte, garantindo uma experiência confortável e segura para o usuário final.

---

## Como reproduzir o projeto

### Pré-requisitos (Bibliotecas Arduino IDE)
* `PubSubClient` (por Nick O'Leary)
* `Adafruit SSD1306`
* `Adafruit GFX Library`

### Passo a Passo da Instalação
1. Clone este repositório para o seu computador.
2. Abra o arquivo `main.cpp` (ou `.ino`) no Arduino IDE.
3. Modifique as variáveis `WIFI_SSID` e `WIFI_PASSWORD` nas primeiras linhas do código com as credenciais da sua rede local (Recomendado 2.4GHz).
4. Conecte o ESP32 ao computador e selecione a placa **DOIT ESP32 DEVKIT V1** no Gerenciador de Placas.
5. Compile e faça o Upload do código.
6. Em um computador ou celular, abra o **MQTT Explorer**.
7. Configure uma nova conexão para o host `broker.hivemq.com`, porta `1883`, sem criptografia.
8. Na aba *Advanced*, adicione a assinatura do tópico genérico `ieee/cefet/ruido/#`.
9. Clique em conectar e visualize a plotagem gráfica do ruído em tempo real!
