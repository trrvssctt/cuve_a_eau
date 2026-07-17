/*
  CuveGuard - Firmware ESP32 (projet Wokwi)
  Master 1 IA - DIT - Projet IoT "CuveGuard"

  Pilotage de la pompe :
   -> La pompe est commandée UNIQUEMENT par RPC (setPump), quelle que soit
      l'origine :
        - agent Python (mode auto, seuils 30 % / 90 % avec hystérésis) ;
        - dashboard ThingsBoard (mode manuel).
   -> Le firmware ne fait plus d'auto-contrôle local : la logique auto est
      entièrement portée par l'agent Python (conforme au sujet). Ainsi
      l'état réel `pumpOn` reflète toujours la dernière commande reçue.
   -> `manualMode` reste publié dans la télémétrie pour que l'agent sache
      qu'il doit rester en HOLD (ne pas forcer la pompe).
   -> L'alerte locale (LED rouge + buzzer) reste gérée sur l'ESP32.
*/

#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "DHT.h" // Bibliothèque officielle Adafruit

// ------------------------------------------------------------------
// Configuration réseau et ThingsBoard
// ------------------------------------------------------------------
const char* WIFI_SSID     = "Wokwi-GUEST";
const char* WIFI_PASSWORD = "";
const char* TB_SERVER     = "eu.thingsboard.cloud";
const int   TB_PORT       = 1883;
const char* TB_TOKEN      = "ButC7lUC5TSw7FA4DWIV";

// CORRECTION : Adapté aux dimensions de la simulation Wokwi (~377 cm)
const float DIST_CUVE_VIDE_CM   = 400.0; 
const float DIST_CUVE_PLEINE_CM = 20.0;

// ------------------------------------------------------------------
// Affectation des Broches
// ------------------------------------------------------------------
#define PIN_TRIG      5
#define PIN_ECHO      18
#define PIN_DHT       4
#define PIN_RELAY     26
#define PIN_LED_PUMP  27
#define PIN_LED_ALERT 25
#define PIN_BUZZER    33

#define DHTTYPE DHT22

WiFiClient espClient;
PubSubClient mqtt(espClient);
DHT dht(PIN_DHT, DHTTYPE);

bool pumpOn = false;
bool manualMode = false;

unsigned long lastTelemetry = 0;
// 3 s : cadence sûre pour le tier gratuit d'eu.thingsboard.cloud. Publier
// plus vite (1 s) fait dépasser la limite de débit -> la session du device
// est bridée et les RPC entrants (setPump/setManualMode) sont perdus.
const unsigned long TELEMETRY_INTERVAL_MS = 3000;
unsigned long lastMqttAttempt = 0;

// Dernières valeurs DHT valides (le DHT22 ne s'échantillonne qu'à ~2 s ;
// on conserve la dernière lecture correcte au lieu de retomber à 0).
float lastTemp = 24.0;
float lastHum  = 40.0;

// ------------------------------------------------------------------
// Connexion Wi-Fi
// ------------------------------------------------------------------
void connectWifi() {
  Serial.print("Connexion Wi-Fi");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
  }
  Serial.println(" OK");
  Serial.print("Adresse IP : ");
  Serial.println(WiFi.localIP());
}

// ------------------------------------------------------------------
// Capteur Ultrasons
// ------------------------------------------------------------------
float mesurerDistanceCm() {
  digitalWrite(PIN_TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(PIN_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);

  long duree = pulseIn(PIN_ECHO, HIGH, 30000UL);
  if (duree == 0) return DIST_CUVE_VIDE_CM;
  return duree * 0.034f / 2.0f;
}

float distanceVersPourcentage(float distanceCm) {
  float pct = 100.0f * (DIST_CUVE_VIDE_CM - distanceCm) / (DIST_CUVE_VIDE_CM - DIST_CUVE_PLEINE_CM);
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  return pct;
}

int calculerAlertLevel(float pct) {
  if (pct < 15.0f) return 2;   // Critique
  if (pct < 30.0f) return 1;   // Attention
  return 0;                    // Normal
}

// ------------------------------------------------------------------
// Contrôle des Actionneurs
// ------------------------------------------------------------------
void appliquerEtatPompe(bool on) {
  pumpOn = on;
  digitalWrite(PIN_RELAY, pumpOn ? LOW : HIGH);
  digitalWrite(PIN_LED_PUMP, pumpOn ? HIGH : LOW);
}

void appliquerAlerteLocale(int alertLevel) {
  bool critique = (alertLevel == 2);
  digitalWrite(PIN_LED_ALERT, critique ? HIGH : LOW);
  digitalWrite(PIN_BUZZER, critique ? HIGH : LOW);
}

// ------------------------------------------------------------------
// Gestion des Requêtes RPC (ThingsBoard)
// ------------------------------------------------------------------
bool extraireValeurBool(JsonVariant params) {
  if (params.is<bool>()) return params.as<bool>();
  if (params.is<JsonObject>()) return params["value"] | false;
  return false;
}

void callback(char* topic, byte* payload, unsigned int length) {
  String topicStr = String(topic);
  String msg;
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];

  Serial.println("RPC reçu [" + topicStr + "] : " + msg);

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, msg);
  if (err) return;

  String method = doc["method"] | "";

  int idx = topicStr.lastIndexOf('/');
  String requestId = (idx > 0) ? topicStr.substring(idx + 1) : "";
  String responseTopic = "v1/devices/me/rpc/response/" + requestId;

  JsonDocument resp;

  if (method == "setPump") {
    appliquerEtatPompe(extraireValeurBool(doc["params"]));
    resp["pumpOn"]     = pumpOn;
    resp["success"]    = true;
  }
  else if (method == "setManualMode") {
    manualMode = extraireValeurBool(doc["params"]);
    resp["manualMode"] = manualMode;
    resp["success"]    = true;
  }
  else if (method == "getState") {
    resp["pumpOn"]     = pumpOn;
    resp["manualMode"] = manualMode;
  }
  else {
    resp["success"]    = false;
  }

  if (requestId.length() > 0) {
    char buffer[128];
    size_t n = serializeJson(resp, buffer);
    mqtt.publish(responseTopic.c_str(), buffer, n);
  }
}

// ------------------------------------------------------------------
// Gestion de la Connexion MQTT
// ------------------------------------------------------------------
void connectMqtt() {
  Serial.print("Tentative de connexion MQTT...");
  if (mqtt.connect("ESP32CuveGuard", TB_TOKEN, NULL)) {
    Serial.println(" OK !");
    mqtt.subscribe("v1/devices/me/rpc/request/+");
  } else {
    Serial.print(" Échec, code d'erreur : ");
    Serial.println(mqtt.state());
  }
}

void publierTelemetrie(float distanceCm, float waterLevelPct, float temperature, float humidity, int alertLevel) {
  JsonDocument doc;
  doc["distanceCm"]    = distanceCm;
  doc["waterLevelPct"] = waterLevelPct;
  doc["temperature"]   = temperature;
  doc["humidity"]      = humidity;
  doc["pumpOn"]        = pumpOn;
  doc["manualMode"]    = manualMode;
  doc["alertLevel"]    = alertLevel;

  char buffer[256];
  size_t n = serializeJson(doc, buffer);
  mqtt.publish("v1/devices/me/telemetry", buffer, n);
  Serial.print("Télémesure transmise : ");
  Serial.println(buffer);
}

// ------------------------------------------------------------------
// Initialisation et Boucle Principale
// ------------------------------------------------------------------
void setup() {
  Serial.begin(115200);

  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);
  pinMode(PIN_RELAY, OUTPUT);
  pinMode(PIN_LED_PUMP, OUTPUT);
  pinMode(PIN_LED_ALERT, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);

  appliquerEtatPompe(false);
  digitalWrite(PIN_LED_ALERT, LOW);
  digitalWrite(PIN_BUZZER, LOW);

  dht.begin();

  connectWifi();
  mqtt.setServer(TB_SERVER, TB_PORT);
  mqtt.setCallback(callback);
  mqtt.setBufferSize(512);

  connectMqtt();
}

void loop() {
  if (!mqtt.connected()) {
    if (millis() - lastMqttAttempt > 5000) {
      lastMqttAttempt = millis();
      connectMqtt();
    }
  } else {
    mqtt.loop();
  }

  if (millis() - lastTelemetry >= TELEMETRY_INTERVAL_MS) {
    lastTelemetry = millis();

    float distanceCm    = mesurerDistanceCm();
    float waterLevelPct = distanceVersPourcentage(distanceCm);

    float temperature = dht.readTemperature();
    float humidity    = dht.readHumidity();
    // Garde la dernière valeur valide si la lecture échoue (NaN)
    if (isnan(temperature)) temperature = lastTemp; else lastTemp = temperature;
    if (isnan(humidity))    humidity    = lastHum;  else lastHum  = humidity;

    int alertLevel = calculerAlertLevel(waterLevelPct);
    appliquerAlerteLocale(alertLevel);

    // La pompe n'est pilotée que par RPC (agent Python en auto, dashboard en
    // manuel). Aucun auto-contrôle local : voir en-tête du fichier.

    if (mqtt.connected()) {
      publierTelemetrie(distanceCm, waterLevelPct, temperature, humidity, alertLevel);
    }
  }
}