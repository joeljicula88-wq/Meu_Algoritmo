#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_MLX90614.h>
#include <MAX30105.h>
#include <LiquidCrystal_I2C.h>
#include <ArduinoJson.h>
#include "heartRate.h"
#include "spo2_algorithm.h"

// ================= CONFIGURAÇÕES WiFi =================
const char* ssid = "NOME DA REDE Wi-Fi";
const char* password = "A senha da rede";

// ================= SERVIDOR =================
String serverURL = "http://192.168.0.12:3000";

// ================= PINOS =================
#define LED_VERMELHO 27
#define LED_LARANJA 26
#define LED_AMARELO 25
#define LED_VERDE 33
#define LED_AZUL 32
#define BUZZER_PIN 13

#define MAX_SDA_PIN 18
#define MAX_SCL_PIN 19




// ================= HARDWARE =================
Adafruit_MLX90614 mlx = Adafruit_MLX90614();
MAX30105 particleSensor;
LiquidCrystal_I2C lcd(0x27, 16, 2);

// ================= VARIÁVEIS DOS SENSORES =================
float bodyTemp = 36.5;
int heartRate = 0;
int spo2 = 0;
int respRate = 0;

// ================= CONTROLE MULTI-CORE =================
SemaphoreHandle_t xSensorMutex;

// ================= VARIÁVEIS DO BACKEND AJUSTADAS =================
int serverPainLevel = 0;         // Mapeado de: "dor"
int serverWaitTime = 0;          // Mapeado de: "waitTime"
String serverClassification = "";// Mapeado de: "classification"
bool alertTriggered = false;     // Mapeado de: "alert_triggered"

// ================= TEMPORIZADORES NÃO BLOQUEANTES =================
int displayPage = 0;
unsigned long lastPageChange = 0;
const unsigned long pageInterval = 2000;

unsigned long lastPostTime = 0;
const unsigned long postInterval = 5000;

unsigned long lastWiFiCheck = 0;
const unsigned long wiFiCheckInterval = 10000;

// BUFFERS SINAIS VITAIS
byte rates[8];
byte rateSpot = 0;
long lastBeat = 0;
float beatsPerMinute;
int beatAvg;


// ================= CONTROLE DO BUZZER RÍTMICO =================
unsigned long lastBuzzToggle = 0;
bool buzzState = false;

unsigned long buzzInterval = 0; // intervalo em ms entre mudanças de estado


// Buffer para Frequência Respiratória (FR)
#define RESP_WINDOW 150
float respBuffer[RESP_WINDOW];
int respIndex = 0;

// Variables de suavização de SpO2 contínuo
float currentSpo2Calc = 0;

// ================= PROTÓTIPOS =================
void handleWiFiAsync();
void sampleSensors();
void sendDataToServer();
void queryServerCommands();
void handleIndicators();
void updateLcdScreen();
void resetAllIndicators();


void TaskSensorCardiaco(void *pvParameters) {
    (void) pvParameters;
    for (;;) {
        particleSensor.check();

        Serial.print("Samples disponiveis: ");
        Serial.println(particleSensor.available());

        sampleSensors(); // Lê o sensor continuamente no Core 0
        vTaskDelay(10 / portTICK_PERIOD_MS); // Pequena pausa necessária para o sistema
   
    }
}




// ================= SETUP =================
void setup() {
    Serial.begin(115200);
    Serial.println("\n--- ESP32 INICIANDO SISTEMA DE TRIAGEM ---");

    pinMode(LED_VERMELHO, OUTPUT);
    pinMode(LED_LARANJA, OUTPUT);
    pinMode(LED_AMARELO, OUTPUT);
    pinMode(LED_VERDE, OUTPUT);
    pinMode(LED_AZUL, OUTPUT);
    pinMode(BUZZER_PIN, OUTPUT);
    resetAllIndicators();

    // Teste de boot
    digitalWrite(LED_VERDE, HIGH);
    digitalWrite(BUZZER_PIN, HIGH);
    delay(100);
    digitalWrite(LED_VERDE, LOW);
    digitalWrite(BUZZER_PIN, LOW);

    Wire.begin(21, 22, 100000); 

    

    lcd.init();
    lcd.backlight();
    lcd.setCursor(0, 0);
    lcd.print("SISTEMA TRIAGEM");
    lcd.setCursor(0, 1);
    lcd.print("Iniciando...");

    if (!mlx.begin()) {
        Serial.println("ERRO: MLX90614 nao detectado!");
    }

    Wire1.begin(MAX_SDA_PIN, MAX_SCL_PIN, 400000);

    if (!particleSensor.begin(Wire1, I2C_SPEED_FAST)) {
        Serial.println("ERRO CRÍTICO: MAX30102 nao detectado!");
        lcd.clear();
        lcd.print("ERRO: MAX30102");
        while(1) { delay(100); }
    } else {
        // Configuração recomendada estável do sensor
        byte ledBrightness = 100;
        byte sampleAverage = 4;
        byte ledMode = 2; // Vermelho + IR
        int sampleRate = 100;
        int pulseWidth = 411;
        int adcRange = 16384;
        particleSensor.setup(ledBrightness, sampleAverage, ledMode, sampleRate, pulseWidth, adcRange);
         xSensorMutex = xSemaphoreCreateMutex(); // Inicializa o cadeado de segurança
    if (xSensorMutex != NULL) {
        xTaskCreatePinnedToCore(
            TaskSensorCardiaco,   // Função criada no Passo 2
            "TaskSensor",         // Nome da tarefa
            4096,                 // Memória alocada
            NULL,                 // Parâmetros
            2,                    // Prioridade alta
            NULL,                 // Ponteiro de controle
            0                     // Força a execução no CORE 0
        );

     }

    }

    WiFi.begin(ssid, password);
    
    lcd.clear();
    lcd.print("SISTEMA PRONTO");
    delay(1000);
    lcd.clear();
}


// ================= LOOP PRINCIPAL =================
void loop() {
    unsigned long currentMillis = millis();

    // 1. Mudança sequencial estrita das 6 páginas
    if (currentMillis - lastPageChange >= pageInterval) {
        lastPageChange = currentMillis;
        displayPage = (displayPage + 1) % 6; 
        lcd.clear(); 
    }

    // 2. Atualizar display constantemente
    updateLcdScreen();

    // 3. O 'sampleSensors()' FOI REMOVIDO DAQUI! Ele agora roda isolado no Core 0.

    // 4. Gestão assíncrona do WiFi
    if (currentMillis - lastWiFiCheck >= wiFiCheckInterval) {
        lastWiFiCheck = currentMillis;
        handleWiFiAsync();
    }

    // 5. Chamadas HTTP sincronizadas (Apenas se houver WiFi)
    if (WiFi.status() == WL_CONNECTED) {
        if (currentMillis - lastPostTime >= postInterval) {
            lastPostTime = currentMillis;
            sendDataToServer();
            queryServerCommands();
        }
    }

    // 6. Ativar LEDs e Alerta Sonoro
    handleIndicators();
}

void handleWiFiAsync() {
    if (WiFi.status() != WL_CONNECTED) {
        WiFi.disconnect();
        WiFi.begin(ssid, password);
    }
}

void sampleSensors() {

   // --- LEITURA DA TEMPERATURA E CALIBRAÇÃO ---
  
const int NUM_AMOSTRAS = 10;
float somaTemp = 0;
int amostrasValidas = 0;

// Coleta e filtragem das amostras do sensor
for (int i = 0; i < NUM_AMOSTRAS; i++) {
    float t_obj = mlx.readObjectTempC();

    // Valida se a leitura não é um erro (NaN) e está dentro da faixa aceitável
    if (!isnan(t_obj) && t_obj > 10.0 && t_obj < 50.0) {
        somaTemp += t_obj;
        amostrasValidas++;
    }
   
 }

 // Processamento da média e aplicação da curva de calibração
 if (amostrasValidas > 0) {
    float mediaTemp = somaTemp / (float)amostrasValidas;

    // Fórmula de ajuste (objeto -> corpo)
    bodyTemp = (mediaTemp * 1.02) + 1.82; 
 }


    // --- LEITURA DO MAX30102  ---
    
    static uint32_t lastIRValue = 0; // Para debug
    
    while (particleSensor.available()) {
        uint32_t irValue = particleSensor.getFIFOIR();
        uint32_t redValue = particleSensor.getFIFORed();
        particleSensor.nextSample(); // Avança para próxima amostra
        
        // Debug a cada ~50 amostras
        static int debugCounter = 0;
        if (debugCounter++ % 50 == 0) {
            Serial.print("IR=");
            Serial.print(irValue);
            Serial.print(" Red=");
            Serial.println(redValue);
        }
        
        lastIRValue = irValue;
        
        // Verificação de dedo presente (CONTINUA em vez de RETURN)
        if (irValue < 10000) {
            // Apenas reseta a FC se ficar muito tempo sem dedo
            static unsigned long lastFingerTime = 0;
            if (millis() - lastFingerTime > 3000) { // 3 segundos sem dedo
                if (xSemaphoreTake(xSensorMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
                    heartRate = 0;
                    spo2 = 0;
                    respRate = 0;
                    currentSpo2Calc = 0;
                    beatAvg = 0;
                    for(int i = 0; i < 8; i++) rates[i] = 0;
                    xSemaphoreGive(xSensorMutex);
                }
                lastFingerTime = millis();
            }
            continue; // Pula esta amostra, mas processa as próximas
        }
        
        // --- CÁLCULO DE FREQUÊNCIA CARDÍACA ---
        if (checkForBeat(irValue) == true) {
            long delta = millis() - lastBeat;
            lastBeat = millis();
            
            if (delta > 300 && delta < 2000) { // Filtro de intervalo válido (30-200 BPM)
                float bpm = 60000.0 / (float)delta;
                
                Serial.print("BATIDA! Delta=");
                Serial.print(delta);
                Serial.print("ms BPM=");
                Serial.println(bpm);
                
                if (bpm >= 40 && bpm <= 200) {
                    // Adiciona ao buffer circular
                    rates[rateSpot] = (byte)bpm;
                    rateSpot = (rateSpot + 1) % 8;
                    
                    // Calcula média das últimas 8 batidas
                    int soma = 0;
                    int validas = 0;
                    for (int i = 0; i < 8; i++) {
                        if (rates[i] > 0) {
                            soma += rates[i];
                            validas++;
                        }
                    }
                    
                    if (validas > 0) {
                        if (xSemaphoreTake(xSensorMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
                            heartRate = soma / validas;
                            xSemaphoreGive(xSensorMutex);
                        }
                    }
                }
            }
        }
        
        // --- CÁLCULO DE SpO2  ---
        if (redValue > 0 && irValue > 10000) {
            float rRatio = (float)redValue / (float)irValue;
            float spo2Estimado = 104.0 - (17.0 * rRatio);
            
            if (spo2Estimado > 100.0) spo2Estimado = 100.0;
            if (spo2Estimado < 60.0) spo2Estimado = 60.0;
            
            if (currentSpo2Calc == 0) currentSpo2Calc = spo2Estimado;
            else currentSpo2Calc = (currentSpo2Calc * 0.95) + (spo2Estimado * 0.05);
            
            if (xSemaphoreTake(xSensorMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
                spo2 = (int)currentSpo2Calc;
                xSemaphoreGive(xSensorMutex);
            }
        }
        
        // --- ESTIMATIVA DE FREQUÊNCIA RESPIRATÓRIA  ---
        respBuffer[respIndex] = (float)irValue;
        respIndex = (respIndex + 1) % RESP_WINDOW;
        
        static unsigned long lastRespCalc = 0;
        if (millis() - lastRespCalc > 5000) { // Recalcula FR a cada 5 segundos
            float somaResp = 0;
            for (int i = 0; i < RESP_WINDOW; i++) {
                somaResp += respBuffer[i];
            }
            float mediaResp = somaResp / (float)RESP_WINDOW;
            
            int cruzamentos = 0;
            for (int i = 1; i < RESP_WINDOW; i++) {
                if (respBuffer[i-1] < mediaResp && respBuffer[i] >= mediaResp) {
                    cruzamentos++;
                }
            }
            
            float estimatedFR = cruzamentos * 2.5;
            if (estimatedFR >= 8 && estimatedFR <= 30) {
                respRate = (int)estimatedFR;
            } else if (respRate == 0) {
                respRate = 16;
            }
            lastRespCalc = millis();
        }
    }
}
// ================= FUNÇÃO DO DISPLAY COM AS 6 PÁGINAS =================
void updateLcdScreen() {
    // Linha Superior Estática
    lcd.setCursor(0, 0);
    switch(displayPage) {
        case 0: lcd.print("[1/6] TEMP. CORP"); break;
        case 1: lcd.print("[2/6] FREQ. CARD"); break;
        case 2: lcd.print("[3/6] SATURACAO "); break;
        case 3: lcd.print("[4/6] FREQ. RESP"); break;
        case 4: lcd.print("[5/6] ESCALA DOR"); break; // Página Dor
        case 5: lcd.print("[6/6] T. ESPERA "); break; // Página Tempo de Espera
    }

    // Linha Inferior Dinâmica
    lcd.setCursor(0, 1);
    switch(displayPage) {
        case 0:
            lcd.print("Temp: "); lcd.print(bodyTemp, 1); lcd.print(" C      "); 
            break;
        case 1:
            lcd.print("FC:   "); lcd.print(heartRate); lcd.print(" bpm    ");
            break;
        case 2:
            lcd.print("SpO2: "); lcd.print(spo2); lcd.print(" %      ");
            break;
        case 3:
            lcd.print("FR:   "); lcd.print(respRate); lcd.print(" rpm    ");
            break;
        case 4:
            lcd.print("Dor:  "); lcd.print(serverPainLevel); lcd.print(" / 10    "); 
            break;
        case 5:
            lcd.print("Tempo: "); lcd.print(serverWaitTime); lcd.print(" min    "); 
            break;
    }
}

// ================= ENVIO DOS VITAIS (POST) =================
void sendDataToServer() {
    HTTPClient http;
    String requestPath = serverURL + "/esp32/vitals";

    http.begin(requestPath);
    http.setTimeout(1000); 
    http.addHeader("Content-Type", "application/json");

    // --- PROTEÇÃO MULTI-CORE: Coleta segura dos dados ---
    int safeHeartRate = 0;
    int safeSpo2 = 0;

    // Tenta pegar o cadeado por até 10 milissegundos
    if (xSemaphoreTake(xSensorMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        safeHeartRate = heartRate; // Copia o valor calculado pelo Core 0
        safeSpo2 = spo2;           // Copia o SpO2 calculado pelo Core 0
        xSemaphoreGive(xSensorMutex); // Libera o Core 0 imediatamente
    } else {
        // Se o Core 0 estava escrevendo bem nesse instante, usa o último valor da memória
        safeHeartRate = heartRate;
        safeSpo2 = spo2;
    }

    StaticJsonDocument<384> doc;
    doc["temperatura"] = bodyTemp;
    doc["fc"] = safeHeartRate;    // Usa a cópia protegida
    doc["spo2"] = safeSpo2;       // Usa a cópia protegida
    doc["fr"] = respRate;

    String jsonPayload;
    serializeJson(doc, jsonPayload);

    int httpCode = http.POST(jsonPayload); // Esta linha trava o Core 1, mas o Core 0 continua livre!
    if (httpCode == 200) {
        String response = http.getString();
        StaticJsonDocument<512> respDoc;
        DeserializationError error = deserializeJson(respDoc, response);
        
        if (!error) {
            // Captura o estado de alerta e classificação que o ESP32Controller envia no POST
            alertTriggered = respDoc["alert_triggered"] | false;
            if(respDoc.containsKey("current_classification")) {
                serverClassification = respDoc["current_classification"].as<String>();
            }
        }
    }
    http.end();
}


// ================= LEITURA DOS DADOS (GET) =================
void queryServerCommands() {
    HTTPClient http;
    String requestPath = serverURL + "/api/triage/active";

    http.begin(requestPath);
    http.setTimeout(1000);

    int httpCode = http.GET();
    if (httpCode == 200) {
        String payload = http.getString();
        StaticJsonDocument<512> doc; 
        DeserializationError error = deserializeJson(doc, payload);

        if (!error) {
            serverPainLevel = doc["dor"].as<int>();
            serverWaitTime = doc["waitTime"].as<int>();
            serverClassification = doc["classification"].as<String>();
            
            //  Se a classificação for "Aguardando" ou vazia, desativa o alerta
            if (serverClassification.equalsIgnoreCase("Aguardando") || serverClassification.isEmpty()) {
                alertTriggered = false; // Reseta o alerta
            }
        }
    }
    http.end();
}

// ================= LÓGICA AUTOMÁTICA DOS LEDS E ALERTA =================
void handleIndicators() {
    // --- LEDS (mantido igual, mas com base na classificação) ---
    digitalWrite(LED_VERMELHO, LOW);
    digitalWrite(LED_LARANJA, LOW);
    digitalWrite(LED_AMARELO, LOW);
    digitalWrite(LED_VERDE, LOW);
    digitalWrite(LED_AZUL, LOW);

    if (serverClassification.equalsIgnoreCase("Vermelho") || serverClassification.equalsIgnoreCase("Emergência")) {
        digitalWrite(LED_VERMELHO, HIGH);
    } 
    else if (serverClassification.equalsIgnoreCase("Laranja") || serverClassification.equalsIgnoreCase("Muito Urgente")) {
        digitalWrite(LED_LARANJA, HIGH);
    } 
    else if (serverClassification.equalsIgnoreCase("Amarelo") || serverClassification.equalsIgnoreCase("Urgente")) {
        digitalWrite(LED_AMARELO, HIGH);
    } 
    else if (serverClassification.equalsIgnoreCase("Verde") || serverClassification.equalsIgnoreCase("Pouco Urgente")) {
        digitalWrite(LED_VERDE, HIGH);
    } 
    else if (serverClassification.equalsIgnoreCase("Azul") || serverClassification.equalsIgnoreCase("Não Urgente")) {
        digitalWrite(LED_AZUL, HIGH);
    }

    // --- BUZZER RÍTMICO (NOVO) ---
    // Define o intervalo com base na classificação
    if (serverClassification.equalsIgnoreCase("Vermelho")) {
        buzzInterval = 300; // 150ms ON + 150ms OFF = 300ms ciclo
    } 
    else if (serverClassification.equalsIgnoreCase("Laranja")) {
        buzzInterval = 800; // 400ms ON + 400ms OFF
    } 
    else if (serverClassification.equalsIgnoreCase("Amarelo")) {
        buzzInterval = 1600; // 800ms ON + 800ms OFF
    } 
    else if (serverClassification.equalsIgnoreCase("Verde")) {
        buzzInterval = 1000; // 100ms ON + 900ms OFF (pulso curto)
    } 
    else {
        // Azul, Aguardando ou vazio -> buzzer desligado
        buzzInterval = 0;
    }

    // Se não houver classificação ou for Aguardando, desliga o buzzer
    if (buzzInterval == 0 || serverClassification.equalsIgnoreCase("Aguardando") || serverClassification == "") {
        digitalWrite(BUZZER_PIN, LOW);
        buzzState = false;
        return;
    }

    // Controle do ritmo com millis() (não bloqueante)
    unsigned long currentMillis = millis();
    if (currentMillis - lastBuzzToggle >= buzzInterval / 2) { // metade do ciclo para alternar
        lastBuzzToggle = currentMillis;
        buzzState = !buzzState;
        digitalWrite(BUZZER_PIN, buzzState ? HIGH : LOW);
    }

    // Para classificação Verde, queremos um pulso curto (100ms ON) e depois OFF longo
    // Ajuste adicional: se for Verde, forçamos OFF após 100ms
    if (serverClassification.equalsIgnoreCase("Verde")) {
        if (buzzState && (currentMillis - lastBuzzToggle >= 100)) {
            // Após 100ms ligado, desliga e mantém desligado até o próximo ciclo
            digitalWrite(BUZZER_PIN, LOW);
            buzzState = false;
            // Mas precisamos resetar o timer para o próximo ciclo
            if (currentMillis - lastBuzzToggle >= 900) {
                lastBuzzToggle = currentMillis; // recomeça o ciclo
            }
        }
    }
}
void resetAllIndicators() {

    digitalWrite(LED_VERMELHO, LOW);
    digitalWrite(LED_LARANJA, LOW);
    digitalWrite(LED_AMARELO, LOW);
    digitalWrite(LED_VERDE, LOW);
    digitalWrite(LED_AZUL, LOW);
    digitalWrite(BUZZER_PIN, LOW);
}
