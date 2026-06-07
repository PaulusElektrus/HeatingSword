/* Passwords */
#include "secrets.h"

/* Libraries */
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>

/* JSON Library: https://github.com/bblanchon/ArduinoJson */
#include <ArduinoJson.h>

/* Telegram Bot: https://github.com/witnessmenow/Universal-Arduino-Telegram-Bot */
#include <UniversalTelegramBot.h>

enum heaterStates
{
  HEATER_OFF = 0,
  HEATER_ON = 1,
};

enum controlStates
{
  MANUAL_OFF = 0,
  MANUAL_ON = 1,
  AUTOMATIC = 3,
  ERROR = 12345
};

/* Timing */
#define REPORT_TIME 1000
#define UPDATE_TIME 1000
#define TELEGRAM_UPDATE_TIME 1000

/* Debug Mode*/
#undef DEBUG_MODE

/* Global Variables */
heaterStates state = HEATER_OFF;
heaterStates lastState = HEATER_OFF;
controlStates control = AUTOMATIC;
float powerMeasured = 0.0;

/* JSON & Ethernet */
DynamicJsonDocument jsonDoc(1024);
JsonObject jsonData;
WiFiClient client;
HTTPClient http;

/* Telegram */
WiFiClientSecure secureClient;
UniversalTelegramBot bot(BOT_TOKEN, secureClient);

void setup()
{
  Serial.begin(115200);
  setupWLAN();
  secureClient.setInsecure();
}

void setupWLAN()
{
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  while (WiFi.status() != WL_CONNECTED)
  {
    delay(100);
  }

#ifdef DEBUG_MODE
  Serial.print("\n\nW-Lan connected with IP: ");
  Serial.println(WiFi.localIP());
#endif
}

void loop()
{
  getSetData();
  serialReport();
  checkTelegramBot();
  notifyOnStateChange();
}

void getSetData()
{
  static unsigned long lastReport = millis();

  if (millis() - lastReport >= UPDATE_TIME)
  {
    /* Get Data */
    http.setTimeout(500);
    http.begin(client, SHELLY_IP);
    int httpCode = http.GET();

    if (httpCode == 200)
    {
      String payload = http.getString();
      DeserializationError error = deserializeJson(jsonDoc, payload);

      if (error)
      {
#ifdef DEBUG_MODE
        Serial.print("JSON Deserialization failed: ");
        Serial.println(error.c_str());
#endif
        powerMeasured = float(ERROR);
      }
      else
      {
        jsonData = jsonDoc.as<JsonObject>();
        powerMeasured = jsonData["total_act_power"];
      }
    }
    else
    {
#ifdef DEBUG_MODE
      Serial.print("HTTP GET failed, code: ");
      Serial.println(httpCode);
#endif
      powerMeasured = float(ERROR);
    }

    /* Set Data */
    switch (control)
    {
    case AUTOMATIC:
      Serial.print(powerMeasured);
      break;
    case MANUAL_ON:
      Serial.print(float(-ERROR));
      break;
    case MANUAL_OFF:
    case ERROR:
    default:
      Serial.print(float(ERROR));
      break;
    }
    if (Serial.available())
    {
      int recv = Serial.parseInt();
      if (recv == 0)
      {
        state = HEATER_OFF;
      }
      else
      {
        state = HEATER_ON;
      }
    }
    http.end();
    lastReport = millis();
  }
}

void serialReport()
{
#ifdef DEBUG_MODE
  static unsigned long lastReport = millis();

  if (millis() - lastReport >= REPORT_TIME)
  {
    String output;
    output += "Heater State:\n";
    output += state;
    output += "\nJSON:\n";
    for (JsonPair kv : jsonData)
    {
      output += kv.key().c_str();
      output += ": ";
      output += kv.value().as<String>();
      output += "\n";
    }
    Serial.println(output);
    lastReport = millis();
  }
#endif
}

void checkTelegramBot()
{
  static unsigned long lastTelegramCheck = millis();

  if (millis() - lastTelegramCheck < TELEGRAM_UPDATE_TIME)
    return;
  lastTelegramCheck = millis();

  int numNewMessages = bot.getUpdates(bot.last_message_received + 1);

  for (int i = 0; i < numNewMessages; i++)
  {
    String chatId = bot.messages[i].chat_id;
    String text = bot.messages[i].text;

    if (chatId != NOTIFY_USER_ID)
      return;

    if (text.startsWith("/EIN"))
    {
      control = MANUAL_ON;
    }

    else if (text.startsWith("/AUS"))
    {
      control = MANUAL_OFF;
    }

    else if (text.startsWith("/AUTOMATIK"))
    {
      control = AUTOMATIC;
    }

    else if (text.startsWith("/MANUELL"))
    {
      control = MANUAL_OFF;
    }
    String msg = "📊 Aktueller /Status:\n\n";
    msg += "Heizschwert: ";
    msg += (state == HEATER_ON) ? "🟢" : "🔴";
    msg += "\n\nAutomatik:     ";
    msg += (control == AUTOMATIC) ? "🟢     /MANUELL" : "🔴     /AUTOMATIK";
    if (control != AUTOMATIC)
    {
      msg += "\nManuell:         ";
      msg += (control == MANUAL_ON) ? "🟢     /AUS" : "🔴     /EIN";
    }

    msg += "\n\nAktuelle Leistung: ";
    msg += String(powerMeasured, 1);
    msg += " W";
    bot.sendMessage(chatId, msg, "");
  }
}

  void notifyOnStateChange()
{
    if (state != lastState)
    {
      String msg = "🔔 Heizschwert: ";
      msg += (state == HEATER_ON) ? "🟢" : "🔴";
      bot.sendMessage(NOTIFY_USER_ID, msg, "");
      lastState = state;
    }
}
