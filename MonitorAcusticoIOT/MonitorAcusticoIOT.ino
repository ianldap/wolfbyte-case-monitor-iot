/**
 * @file main.cpp
 * @brief Monitor IoT de Conforto Acústico e Acessibilidade
 * @author Equipe WolfByte (IEEE CEFET/RJ)
 * @details Este projeto utiliza um ESP32 e um microfone MAX4466 para captar ruídos 
 * em ambientes. O sinal é processado via cálculo RMS para determinar a energia real do som.
 * Os dados são classificados (Calmo, Moderado, Ruidoso) e publicados em tempo real 
 * via protocolo MQTT para visualização em dashboards, auxiliando pessoas com 
 * hipersensibilidade auditiva.
 */

#include <Arduino.h>
#include <math.h>
#include <WiFi.h>
#include <PubSubClient.h> // Biblioteca para comunicação MQTT
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ---------------- Configurações Wi-Fi e MQTT ----------------
const char* WIFI_SSID = "NOME_DA_SUA_REDE";      // ATENÇÃO: Preencher antes de gravar
const char* WIFI_PASSWORD = "SENHA_DA_SUA_REDE"; // ATENÇÃO: Preencher antes de gravar

// Usando um Broker público para facilitar a demonstração
const char* MQTT_BROKER = "broker.hivemq.com"; 
const int MQTT_PORT = 1883; 
const char* TOPIC_RMS = "ieee/cefet/ruido/rms";
const char* TOPIC_STATE = "ieee/cefet/ruido/estado";

WiFiClient espClient;
PubSubClient mqttClient(espClient);

// ---------------- Mapeamento de Hardware ----------------
#define MIC_PIN 34                 // Pino ADC1 onde o MAX4466 está conectado
#define OLED_SDA 21                // Pino de dados do I2C
#define OLED_SCL 22                // Pino de clock do I2C
#define SCREEN_WIDTH 128           // Largura do display OLED (pixels)
#define SCREEN_HEIGHT 64           // Altura do display OLED (pixels)
#define OLED_ADDRESS 0x3C          // Endereço padrão de displays OLED SSD1306
#define OLED_RESET -1              // -1 indica que o display usa o reset do próprio ESP32

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ---------------- Parâmetros de Amostragem ----------------
constexpr uint32_t SAMPLE_RATE_HZ = 8000; // Lê o microfone 8.000 vezes por segundo
constexpr uint32_t SAMPLE_WINDOW_MS = 1000; // Janela de tempo de 1 segundo (1000 ms)
// Quantidade de amostras que cabem em 1 segundo (8000 amostras)
constexpr uint32_t SAMPLES_PER_WINDOW = SAMPLE_RATE_HZ * SAMPLE_WINDOW_MS / 1000;
// Intervalo exato em microssegundos entre cada leitura (125 us)
constexpr uint32_t SAMPLE_PERIOD_US = 1000000UL / SAMPLE_RATE_HZ;

// Arrays para calcular as médias móveis ao longo de 1 minuto
constexpr size_t MINUTE_WINDOW_SIZE = 60;
float minuteReadings[MINUTE_WINDOW_SIZE] = {0};
size_t minuteWriteIndex = 0;
size_t minuteValidCount = 0;
float minuteAverageRms = 0.0f;
float overallAverageRms = 0.0f;
uint32_t totalValidReadings = 0;

// ---------------- Parâmetros de Classificação Acústica ----------------
// Estes limites representam a energia relativa do som (RMS). 
// Calibrados empiricamente: 150 representa ruído ambiente basal e 250 picos sonoros.
constexpr float RMS_CALM_TO_MODERATE = 150.0f;
constexpr float RMS_MODERATE_TO_NOISY = 250.0f; // Ajuste isso dependendo de quanto dá quando você bate palma
constexpr float HYSTERESIS_FACTOR = 0.80f; // Histerese (80%) para evitar que o status fique "piscando" no limite

enum EnvironmentState : uint8_t { CALMO = 0, MODERADO = 1, RUIDOSO = 2 };
EnvironmentState environmentState = CALMO;

constexpr uint32_t WIFI_RETRY_INTERVAL_MS = 10000; // Tenta reconectar o Wi-Fi a cada 10s se cair
constexpr uint32_t MQTT_INTERVAL_MS = 2000;        // Envia dados para o MQTT a cada 2 segundos

uint32_t lastWifiAttemptMs = 0;
uint32_t lastMqttWriteMs = 0;
float latestRms = 0.0f;
bool hasValidReading = false;
bool oledReady = false;

// ---------------- Funções Utilitárias ----------------

// Converte o estado enum para String visual
const char* stateName(EnvironmentState state) {
  switch (state) {
    case CALMO: return "CALMO";
    case MODERADO: return "MODERADO";
    case RUIDOSO: return "RUIDOSO";
    default: return "INDEFINIDO";
  }
}

/**
 * @brief Calcula a energia do som (RMS) removendo a tensão contínua (DC Offset).
 * @return float Valor do RMS acústico calculado.
 */
float readMicrophoneRms() {
  double sum = 0.0;
  double sumSquares = 0.0;
  uint32_t nextSampleUs = micros();

  // Coleta 8000 amostras cravadas no intervalo de tempo exato
  for (uint32_t i = 0; i < SAMPLES_PER_WINDOW; ++i) {
    // Espera ativa: segura o loop até dar o tempo exato (125 us) da próxima leitura
    while ((int32_t)(micros() - nextSampleUs) < 0) { } 
    
    const uint16_t sample = analogRead(MIC_PIN); // Lê o sensor (0 a 4095)
    sum += sample;                               // Soma os valores
    sumSquares += (double)sample * (double)sample; // Soma os quadrados (para o RMS)
    nextSampleUs += SAMPLE_PERIOD_US;
  }

  // A variância matemática remove automaticamente o "DC Offset" (tensão constante) do microfone
  const double mean = sum / SAMPLES_PER_WINDOW;
  double variance = (sumSquares / SAMPLES_PER_WINDOW) - (mean * mean);
  if (variance < 0.0) variance = 0.0; // Proteção contra erros de arredondamento do float
  
  return (float)sqrt(variance); // A raiz da variância resulta no valor RMS puramente acústico
}

/**
 * @brief Classifica o ambiente com histerese. 
 * A histerese impede mudanças frenéticas de status quando o som oscila exatamente no limite.
 */
EnvironmentState classifyEnvironment(float rms) {
  const float calmReturnLimit = RMS_CALM_TO_MODERATE * HYSTERESIS_FACTOR;
  const float noisyReturnLimit = RMS_MODERATE_TO_NOISY * HYSTERESIS_FACTOR;

  switch (environmentState) {
    case CALMO:
      if (rms >= RMS_MODERATE_TO_NOISY) environmentState = RUIDOSO;
      else if (rms >= RMS_CALM_TO_MODERATE) environmentState = MODERADO;
      break;
    case MODERADO:
      if (rms >= RMS_MODERATE_TO_NOISY) environmentState = RUIDOSO;
      else if (rms < calmReturnLimit) environmentState = CALMO;
      break;
    case RUIDOSO:
      if (rms < calmReturnLimit) environmentState = CALMO;
      else if (rms < noisyReturnLimit) environmentState = MODERADO;
      break;
  }
  return environmentState;
}

// Atualiza as estatísticas de tempo (média de 1 minuto e média geral)
void updateAverages(float rms) {
  ++totalValidReadings;
  if (totalValidReadings == 1) overallAverageRms = rms;
  else overallAverageRms += (rms - overallAverageRms) / totalValidReadings;

  minuteReadings[minuteWriteIndex] = rms;
  minuteWriteIndex = (minuteWriteIndex + 1) % MINUTE_WINDOW_SIZE;
  if (minuteValidCount < MINUTE_WINDOW_SIZE) ++minuteValidCount;

  float sum = 0.0f;
  for (size_t i = 0; i < minuteValidCount; ++i) sum += minuteReadings[i];
  minuteAverageRms = (minuteValidCount > 0) ? sum / minuteValidCount : 0.0f;
}

// Gerencia o Wi-Fi sem usar delay() para não travar a leitura do sensor
void startOrMaintainWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  const uint32_t now = millis();
  if (now - lastWifiAttemptMs >= WIFI_RETRY_INTERVAL_MS) {
    lastWifiAttemptMs = now;
    Serial.println("Wi-Fi desconectado; tentando reconectar...");
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }
}

// Garante a conexão MQTT gerando um ID de cliente único
void reconnectMQTT() {
  if (!mqttClient.connected()) {
    String clientId = "ESP32Noise-";
    clientId += String(random(0xffff), HEX);
    if (mqttClient.connect(clientId.c_str())) {
      Serial.println("Conectado ao Broker MQTT!");
    }
  }
}

// Empacota e envia os dados para a nuvem
void sendToMQTTIfDue() {
  if (!hasValidReading || WiFi.status() != WL_CONNECTED || !mqttClient.connected()) return;

  const uint32_t now = millis();
  if (now - lastMqttWriteMs < MQTT_INTERVAL_MS) return; // Limita o envio a cada 2 segundos
  lastMqttWriteMs = now;

  char rmsStr[10];
  dtostrf(latestRms, 1, 2, rmsStr); // Converte a variável float para string (texto)
  
  mqttClient.publish(TOPIC_RMS, rmsStr);
  mqttClient.publish(TOPIC_STATE, stateName(environmentState));
  
  Serial.println("Dados enviados ao MQTT com sucesso.");
}

// Renderiza a interface gráfica do display OLED
void drawDisplay() {
  if (!oledReady) return;

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  
  // Título
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("MONITOR ACUSTICO");

  // Estado atual
  display.setCursor(0, 12);
  display.print("Estado: ");
  display.print(stateName(environmentState));

  // Leitura do momento
  display.setCursor(0, 25);
  display.print("RMS rel: ");
  if (hasValidReading) display.print(latestRms, 1);
  else display.print("aguardando");

  // Médias
  display.setCursor(0, 38);
  display.print("Med 1m: ");
  display.print(minuteAverageRms, 1);

  display.setCursor(0, 49);
  display.print("Med ger: ");
  display.print(overallAverageRms, 1);

  // Status de rede
  display.setCursor(0, 58);
  display.print("Net: ");
  if (WiFi.status() == WL_CONNECTED && mqttClient.connected()) display.print("Conectado");
  else display.print("Offline");

  display.display();
}

// ---------------- Ciclo de Inicialização (Setup) ----------------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\nInicializando Monitor Acustico IEEE CEFET...");

  // Configuração do conversor Analógico-Digital (ADC)
  analogReadResolution(12); // Resolução de 12 bits (0 a 4095)
  analogSetPinAttenuation(MIC_PIN, ADC_11db); // Permite ler tensões de até ~3.3V
  pinMode(MIC_PIN, INPUT);

  // Inicialização do Display OLED via protocolo I2C
  Wire.begin(OLED_SDA, OLED_SCL);
  oledReady = display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS);
  if (!oledReady) Serial.println("OLED não encontrado. Operando apenas via nuvem/serial.");

  // Configuração de Rede Wi-Fi
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lastWifiAttemptMs = millis();
  
  // Configuração do Servidor MQTT
  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);

  drawDisplay(); // Desenha a primeira tela inicial
}

// ---------------- Loop Principal ----------------
void loop() {
  // 1. Garante que as conexões de rede estejam vivas
  startOrMaintainWiFi();
  if (WiFi.status() == WL_CONNECTED) {
    reconnectMQTT();
    mqttClient.loop(); // Processa pacotes MQTT pendentes
  }

  // 2. Realiza a leitura matemática do som (trava a execução por 1 segundo)
  latestRms = readMicrophoneRms();
  hasValidReading = true;

  // 3. Atualiza os estados e médias
  classifyEnvironment(latestRms);
  updateAverages(latestRms);

  // 4. Imprime no Serial para debug local
  Serial.printf("RMS: %.2f | Estado: %s | Med 1m: %.2f | Med ger: %.2f\n",
                latestRms, stateName(environmentState), minuteAverageRms, overallAverageRms);

  // 5. Atualiza a nuvem e a interface local
  sendToMQTTIfDue();
  drawDisplay();
}
