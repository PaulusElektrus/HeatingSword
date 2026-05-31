/* ============================================================
   Heizsteuerung – ESP32 (zusammengeführt aus Arduino + ESP8266)
   ============================================================
   Logik:
     • Leistung <= 50 W für 10 s  → Einschalten
     • Leistung >= 50 W für 5 s   → Ausschalten
     • Leistung >= 300 W sofort   → Ausschalten
   ============================================================ */

/* Passwörter / Zugangsdaten */
#include "secrets.h"

/* Libraries */
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <UniversalTelegramBot.h>

/* ── Pinout ───────────────────────────────────────────────── */
#define HEATER_RELAIS 4   // GPIO-Pin des Relais (LOW = EIN)

/* ── Schwellwerte ─────────────────────────────────────────── */
#define POWER_ON_THRESHOLD    30.0f   // W  – Einschalten wenn Leistung UNTER diesem Wert
#define POWER_OFF_THRESHOLD   30.0f   // W  – Ausschalten wenn Leistung ÜBER diesem Wert
#define POWER_OFF_INSTANT    100.0f   // W  – Sofort ausschalten
#define POWER_FAULT_VALUE   9999.0f   // W  – Platzhalterwert bei Messfehler (> POWER_OFF_INSTANT → sicheres Ausschalten)

/* ── Zeitvorgaben ─────────────────────────────────────────── */
#define TIME_TO_ON_MS         5000UL  //  x s unter Einschaltschwelle → EIN
#define TIME_TO_OFF_MS        3000UL  //  x s über Ausschaltschwelle  → AUS
#define HTTP_UPDATE_MS        1000UL  // Shelly-Abfrage-Intervall
#define TELEGRAM_UPDATE_MS    1000UL  // Telegram-Poll-Intervall

/* ── Debug-Modus ─────────────────────────────────────────── */
#undef DEBUG_MODE

/* ── Zustände ────────────────────────────────────────────── */
enum HeaterState  { HEATER_OFF = 0, HEATER_ON = 1 };
enum ControlState { MANUAL_OFF = 0, MANUAL_ON = 1, AUTOMATIC = 3 };

/* ── Globale Variablen ───────────────────────────────────── */
HeaterState  heaterState   = HEATER_OFF;
HeaterState  lastState     = HEATER_OFF;
ControlState controlState  = AUTOMATIC;
float        powerMeasured = 0.0f;

// Zeitstempel für die Zustands-Hysterese
unsigned long conditionSinceOn  = 0;  // seit wann ist Einschalt-Bedingung erfüllt
unsigned long conditionSinceOff = 0;  // seit wann ist Ausschalt-Bedingung erfüllt
bool          trackingOn        = false;
bool          trackingOff       = false;

/* ── Netzwerk / JSON ─────────────────────────────────────── */
DynamicJsonDocument jsonDoc(1024);
WiFiClient          httpClient;
HTTPClient          http;

/* ── Telegram ────────────────────────────────────────────── */
WiFiClientSecure       secureClient;
UniversalTelegramBot   bot(BOT_TOKEN, secureClient);

/* ============================================================
   SETUP
   ============================================================ */
void setup()
{
  Serial.begin(115200);
  pinMode(HEATER_RELAIS, OUTPUT);
  heaterOff();          // sicherer Startzustand
  setupWLAN();
  secureClient.setInsecure();
}

void setupWLAN()
{
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED)
    delay(100);

#ifdef DEBUG_MODE
  Serial.print("\n\nW-LAN verbunden, IP: ");
  Serial.println(WiFi.localIP());
#endif
}

/* ============================================================
   LOOP
   ============================================================ */
void loop()
{
  fetchPower();          // 1. Leistung vom Shelly holen
  applyHeaterLogic();    // 2. Schalt-Logik anwenden
  checkTelegramBot();    // 3. Telegram-Befehle verarbeiten
  notifyOnStateChange(); // 4. Bei Zustandswechsel benachrichtigen

#ifdef DEBUG_MODE
  debugReport();
#endif
}

/* ============================================================
   SHELLY – Leistung abrufen
   ============================================================ */
void fetchPower()
{
  static unsigned long lastFetch = 0;
  if (millis() - lastFetch < HTTP_UPDATE_MS) return;
  lastFetch = millis();

  http.setTimeout(500);
  http.begin(httpClient, SHELLY_IP);
  int code = http.GET();

  if (code == 200)
  {
    String payload = http.getString();
    DeserializationError err = deserializeJson(jsonDoc, payload);
    if (err)
    {
#ifdef DEBUG_MODE
      Serial.print("JSON-Fehler: "); Serial.println(err.c_str());
#endif
      powerMeasured = POWER_FAULT_VALUE;
    }
    else
    {
      powerMeasured = jsonDoc["total_act_power"].as<float>();
    }
  }
  else
  {
#ifdef DEBUG_MODE
    Serial.print("HTTP-Fehler: "); Serial.println(code);
#endif
    powerMeasured = POWER_FAULT_VALUE;
  }
  http.end();
}

/* ============================================================
   HEIZLOGIK
   ============================================================ */
void applyHeaterLogic()
{
  // Im manuellen Modus überschreibt die Vorgabe den Automaten
  if (controlState == MANUAL_ON)  { heaterOn();  return; }
  if (controlState == MANUAL_OFF) { heaterOff(); return; }

  // ── AUTOMATIC ──────────────────────────────────────────────
  unsigned long now = millis();

  switch (heaterState)
  {
    /* ── Warten auf Einschalten ────────────────────────────── */
    case HEATER_OFF:
      if (powerMeasured <= POWER_ON_THRESHOLD)
      {
        if (!trackingOn)
        {
          trackingOn       = true;
          conditionSinceOn = now;
        }
        else if (now - conditionSinceOn >= TIME_TO_ON_MS)
        {
          heaterOn();
        }
      }
      else
      {
        trackingOn = false;  // Bedingung unterbrochen → Timer zurücksetzen
      }
      break;

    /* ── Warten auf Ausschalten ────────────────────────────── */
    case HEATER_ON:
      // Sofort-Ausschaltung bei Spitzenlast
      if (powerMeasured >= POWER_OFF_INSTANT)
      {
        heaterOff();
        break;
      }

      if (powerMeasured >= POWER_OFF_THRESHOLD)
      {
        if (!trackingOff)
        {
          trackingOff       = true;
          conditionSinceOff = now;
        }
        else if (now - conditionSinceOff >= TIME_TO_OFF_MS)
        {
          heaterOff();
        }
      }
      else
      {
        trackingOff = false;  // Bedingung unterbrochen → Timer zurücksetzen
      }
      break;

    default:
      heaterOff();
      break;
  }
}

/* ── Relais-Helfer ─────────────────────────────────────────── */
void heaterOn()
{
  trackingOn  = false;
  trackingOff = false;
  digitalWrite(HEATER_RELAIS, LOW);   // LOW = Relais zieht an
  heaterState = HEATER_ON;
}

void heaterOff()
{
  trackingOn  = false;
  trackingOff = false;
  digitalWrite(HEATER_RELAIS, HIGH);  // HIGH = Relais fällt ab
  heaterState = HEATER_OFF;
}

/* ============================================================
   TELEGRAM – Befehle empfangen
   ============================================================ */
void checkTelegramBot()
{
  static unsigned long lastCheck = 0;
  if (millis() - lastCheck < TELEGRAM_UPDATE_MS) return;
  lastCheck = millis();

  int numNew = bot.getUpdates(bot.last_message_received + 1);

  for (int i = 0; i < numNew; i++)
  {
    String chatId = bot.messages[i].chat_id;
    String text   = bot.messages[i].text;

    if (chatId != NOTIFY_USER_ID) continue;

    if      (text.startsWith("/EIN"))       controlState = MANUAL_ON;
    else if (text.startsWith("/AUS"))       controlState = MANUAL_OFF;
    else if (text.startsWith("/AUTOMATIK")) controlState = AUTOMATIC;
    else if (text.startsWith("/MANUELL"))   controlState = MANUAL_OFF;

    // Status-Antwort senden
    String msg = "📊 Aktueller /Status:\n\n";
    msg += "Heizschwert: ";
    msg += (heaterState == HEATER_ON) ? "🟢" : "🔴";
    msg += "\n\nAutomatik:     ";
    msg += (controlState == AUTOMATIC) ? "🟢     /MANUELL" : "🔴     /AUTOMATIK";
    if (controlState != AUTOMATIC)
    {
      msg += "\nManuell:         ";
      msg += (controlState == MANUAL_ON) ? "🟢     /AUS" : "🔴     /EIN";
    }
    msg += "\n\nAktuelle Leistung: ";
    msg += String(powerMeasured, 1);
    msg += " W";
    bot.sendMessage(chatId, msg, "");
  }
}

/* ============================================================
   TELEGRAM – Benachrichtigung bei Zustandswechsel
   ============================================================ */
void notifyOnStateChange()
{
  if (heaterState != lastState)
  {
    String msg = "🔔 Heizschwert: ";
    msg += (heaterState == HEATER_ON) ? "🟢 Eingeschaltet" : "🔴 Ausgeschaltet";
    msg += "\nLeistung: ";
    msg += String(powerMeasured, 1);
    msg += " W";
    bot.sendMessage(NOTIFY_USER_ID, msg, "");
    lastState = heaterState;
  }
}

/* ============================================================
   DEBUG – Serielle Ausgabe
   ============================================================ */
#ifdef DEBUG_MODE
void debugReport()
{
  static unsigned long lastReport = 0;
  if (millis() - lastReport < 2000) return;
  lastReport = millis();

  Serial.printf("\n[DEBUG] Leistung: %.1f W | Heizung: %s | Modus: %s\n",
    powerMeasured,
    (heaterState  == HEATER_ON)  ? "EIN"     : "AUS",
    (controlState == AUTOMATIC)  ? "Automatik" :
    (controlState == MANUAL_ON)  ? "Manuell-EIN" : "Manuell-AUS");
}
#endif
