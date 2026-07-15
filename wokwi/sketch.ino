// =====================================================================
// CuveGuard — Surveillance d'un reservoir d'eau (Projet IoT M1 IA)
// ESP32 + HC-SR04 (niveau) + DHT22 (temp/hum) + relais (pompe)
// + LED rouge / buzzer (alerte locale niveau < 15 %)
// Telemetrie MQTT -> ThingsBoard | RPC: setPump, setManualMode
// =====================================================================

#define MQTT_MAX_PACKET_SIZE 512

#include <WiFi.h>
#include <PubSubClient.h>
#include <DHTesp.h>
#include <ArduinoJson.h>

#if __has_include("secrets.h")
#include "secrets.h"
#else
#define TB_HOST "eu.thingsboard.cloud"
#define TB_PORT 1883
#define TB_ACCESS_TOKEN "METTRE_LE_TOKEN_DU_DEVICE_ESP32"
#endif

// ---------------- Wi-Fi ----------------
const char* WIFI_SSID = "Wokwi-GUEST";
const char* WIFI_PASS = "";

// ---------------- Brochage ----------------
const int DHT_PIN     = 18;   // DHT22 data
const int TRIG_PIN    = 5;    // HC-SR04 trigger
const int ECHO_PIN    = 17;   // HC-SR04 echo
const int PUMP_PIN    = 26;   // relais IN (pompe)
const int LED_ALERTE  = 25;   // LED rouge (niveau bas)
const int BUZZER_PIN  = 27;   // buzzer (niveau critique)

// ---------------- Geometrie de la cuve ----------------
// Le capteur est fixe au sommet, il mesure la distance jusqu'a la surface.
// Distance grande = cuve vide ; distance petite = cuve pleine.
const float DIST_CUVE_VIDE_CM  = 380.0;  // fond de la cuve (0 %)
const float DIST_CUVE_PLEINE_CM = 20.0;  // surface au maximum (100 %)

// ---------------- Seuils d'alerte ----------------
const float SEUIL_ATTENTION = 30.0;  // alertLevel = 1 en dessous
const float SEUIL_CRITIQUE  = 15.0;  // alertLevel = 2 en dessous (alerte locale)

const unsigned long TELEMETRY_INTERVAL_MS = 5000;

// ---------------- Objets ----------------
DHTesp dhtSensor;
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

// ---------------- Etat ----------------
bool pumpOn = false;
bool manualMode = false;
int  alertLevel = 0;              // 0 normal / 1 attention / 2 critique
unsigned long lastTelemetry = 0;
unsigned long lastBeep = 0;

// =====================================================================
// CONNEXIONS
// =====================================================================
void connectWiFi() {
  Serial.print("Wi-Fi");
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(250);
    Serial.print(".");
    attempts++;
  }
  Serial.println(WiFi.status() == WL_CONNECTED ? " OK" : " ECHEC");
}

bool connectMQTT() {
  if (!WiFi.isConnected()) return false;

  mqttClient.setServer(TB_HOST, TB_PORT);
  mqttClient.setBufferSize(512);

  Serial.print("MQTT");
  if (mqttClient.connect("esp32-cuveguard", TB_ACCESS_TOKEN, nullptr)) {
    mqttClient.subscribe("v1/devices/me/rpc/request/+");
    Serial.println(" OK");
    return true;
  }
  Serial.printf(" rc=%d\n", mqttClient.state());
  return false;
}

// =====================================================================
// CAPTEURS
// =====================================================================
float readDistanceCm() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  // timeout 30 ms (~5 m aller-retour)
  long duration = pulseIn(ECHO_PIN, HIGH, 30000);
  if (duration == 0) return -1;               // pas d'echo
  return duration * 0.0343 / 2.0;             // vitesse du son
}

float distanceToLevelPct(float distanceCm) {
  // Interpolation lineaire entre cuve vide et cuve pleine
  float pct = (DIST_CUVE_VIDE_CM - distanceCm) * 100.0
              / (DIST_CUVE_VIDE_CM - DIST_CUVE_PLEINE_CM);
  if (pct < 0)   pct = 0;
  if (pct > 100) pct = 100;
  return pct;
}

int computeAlertLevel(float levelPct) {
  if (levelPct < SEUIL_CRITIQUE)  return 2;   // critique
  if (levelPct < SEUIL_ATTENTION) return 1;   // attention
  return 0;                                   // normal
}

// =====================================================================
// ALERTE LOCALE (LED rouge + buzzer)
// =====================================================================
void gererAlerteLocale() {
  if (alertLevel == 2) {
    digitalWrite(LED_ALERTE, HIGH);
    // bip intermittent toutes les 2 s, sans bloquer le loop
    if (millis() - lastBeep > 2000) {
      lastBeep = millis();
      tone(BUZZER_PIN, 800, 300);
    }
  } else if (alertLevel == 1) {
    digitalWrite(LED_ALERTE, HIGH);   // LED fixe, pas de buzzer
    noTone(BUZZER_PIN);
  } else {
    digitalWrite(LED_ALERTE, LOW);
    noTone(BUZZER_PIN);
  }
}

// =====================================================================
// POMPE
// =====================================================================
void setPump(bool on) {
  pumpOn = on;
  digitalWrite(PUMP_PIN, on ? HIGH : LOW);
  Serial.printf("[POMPE] %s\n", on ? "ON" : "OFF");
}

// =====================================================================
// RPC (commandes recues de ThingsBoard / agent Python)
// =====================================================================
void sendRpcResponse(const char* requestId, JsonDocument& response) {
  char buffer[128];
  size_t len = serializeJson(response, buffer);
  char topic[64];
  snprintf(topic, sizeof(topic), "v1/devices/me/rpc/response/%s", requestId);
  mqttClient.publish(topic, buffer, len);
}

// Les params peuvent arriver sous forme {"value": true} ou true directement
bool extractBoolParam(JsonVariant params) {
  if (params.is<bool>()) return params.as<bool>();
  return params["value"] | false;
}

void processRpc(const char* requestId, JsonDocument& doc) {
  const char* method = doc["method"] | "";
  JsonVariant params = doc["params"];
  StaticJsonDocument<128> response;

  if (strcmp(method, "setPump") == 0) {
    setPump(extractBoolParam(params));
    response["pumpOn"] = pumpOn;
    Serial.println("[RPC] setPump recu");
  }
  else if (strcmp(method, "setManualMode") == 0) {
    manualMode = extractBoolParam(params);
    response["manualMode"] = manualMode;
    Serial.printf("[RPC] setManualMode = %s\n", manualMode ? "true" : "false");
  }
  else if (strcmp(method, "getState") == 0) {
    response["pumpOn"] = pumpOn;
    response["manualMode"] = manualMode;
  }
  else {
    response["error"] = "methode inconnue";
  }
  sendRpcResponse(requestId, response);
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  const char* prefix = "v1/devices/me/rpc/request/";
  size_t prefixLen = strlen(prefix);
  if (strncmp(topic, prefix, prefixLen) != 0) return;

  const char* requestId = topic + prefixLen;
  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, payload, length)) return;
  processRpc(requestId, doc);
}

// =====================================================================
// TELEMETRIE (cles imposees par le sujet)
// =====================================================================
void publishTelemetry(float distanceCm, float levelPct,
                      float temperature, float humidity) {
  StaticJsonDocument<320> doc;
  doc["distanceCm"]    = distanceCm;
  doc["waterLevelPct"] = levelPct;
  doc["temperature"]   = temperature;
  doc["humidity"]      = humidity;
  doc["pumpOn"]        = pumpOn;
  doc["manualMode"]    = manualMode;
  doc["alertLevel"]    = alertLevel;

  char buffer[320];
  size_t len = serializeJson(doc, buffer);
  mqttClient.publish("v1/devices/me/telemetry", buffer, len);

  Serial.printf("[TELEMETRIE] dist=%.1fcm niveau=%.0f%% T=%.1fC H=%.0f%% pompe=%s mode=%s alerte=%d\n",
                distanceCm, levelPct, temperature, humidity,
                pumpOn ? "ON" : "OFF",
                manualMode ? "MANUEL" : "AUTO",
                alertLevel);
}

// =====================================================================
void setup() {
  Serial.begin(115200);
  delay(100);

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(PUMP_PIN, OUTPUT);
  pinMode(LED_ALERTE, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);

  setPump(false);
  digitalWrite(LED_ALERTE, LOW);

  dhtSensor.setup(DHT_PIN, DHTesp::DHT22);

  connectWiFi();
  mqttClient.setCallback(mqttCallback);
  connectMQTT();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) connectWiFi();

  if (!mqttClient.connected()) {
    mqttClient.setCallback(mqttCallback);
    connectMQTT();
    delay(2000);
    return;
  }

  mqttClient.loop();

  // L'alerte locale est geree en continu (pas seulement a la telemetrie)
  gererAlerteLocale();

  unsigned long now = millis();
  if (now - lastTelemetry < TELEMETRY_INTERVAL_MS) return;
  lastTelemetry = now;

  // --- Lecture capteurs ---
  float distance = readDistanceCm();
  if (distance < 0) {
    Serial.println("HC-SR04: pas d'echo");
    return;
  }

  TempAndHumidity dht = dhtSensor.getTempAndHumidity();
  if (isnan(dht.temperature) || isnan(dht.humidity)) {
    Serial.println("DHT22: erreur lecture");
    return;
  }

  float levelPct = distanceToLevelPct(distance);
  alertLevel = computeAlertLevel(levelPct);

  publishTelemetry(distance, levelPct, dht.temperature, dht.humidity);
}
