/* Pinout */
#define HEATER_RELAIS 4

/* Thresholds */
#define POWER_ON  10.0 /* W */
#define POWER_OFF 30.0 /* W */
#define INSTANT_POWER_OFF 600.0 /* W */

/* Timing */
#define HEATER_ON_TIMES 5          /* times */
#define HEATER_ON_DURATION 1800000 /* msec */
#define HEATER_OFF_TIMES 5         /* times */
#define HEATER_UPDATE_TIME 1000    /* msec */
#define REPORT_TIME 1000           /* msec */

/* Debug Mode */
#undef DEBUG_MODE

enum heaterStates
{
  HEATER_OFF = 0,
  HEATER_ON = 1,
};

/* Global Variables */
heaterStates state = HEATER_OFF;
float powerMeasured = 0.0;
unsigned long heater_on_timer = 0;
unsigned int heater_on_times = 0;
unsigned int heater_off_times = 0;

void setup()
{
  Serial.begin(115200);
  pinMode(HEATER_RELAIS, OUTPUT);
  heaterOff();
}

void loop()
{
  getData();
  setHeater();
#ifdef DEBUG_MODE
  serialReport();
#endif
}

void getData()
{
  if (Serial.available())
  {
    powerMeasured = Serial.parseFloat();
    Serial.print(state);
  }
}

void setHeater()
{
  static unsigned long lastUpdate = millis();

  if (millis() - lastUpdate >= HEATER_UPDATE_TIME)
  {

    switch (state)
    {
    case HEATER_OFF:
      if (powerMeasured <= POWER_ON)
      {
        heater_on_times++;
        if (heater_on_times >= HEATER_ON_TIMES)
        {
          heaterOn();
          break;
        }
      }
      else
      {
        heater_on_times = 0;
      }
      break;

    case HEATER_ON:
      if (powerMeasured >= POWER_OFF)
      {
        heater_off_times++;
        if (heater_off_times >= HEATER_OFF_TIMES)
        {
          heaterOff();
          break;
        }
      }
      else
      {
        heater_off_times = 0;
      }
      if (millis() - heater_on_timer >= HEATER_ON_DURATION)
      {
        heaterOff();
      }
      if (powerMeasured >= INSTANT_POWER_OFF)
      {
        if (millis() - heater_on_timer >= HEATER_UPDATE_TIME)
        {
          heaterOff();
        }
      }
      break;

    default:
      heaterOff();
      break;
    }

    lastUpdate = millis();
  }
}

void serialReport()
{
  static unsigned long lastReport = millis();

  if (millis() - lastReport >= REPORT_TIME)
  {
    String output;
    output += "\nActual Power: ";
    output += powerMeasured;
    output += "\nHeater State: ";
    output += state;
    output += "\n";
    if (state == HEATER_ON)
    {
      output += "Running: ";
      output += (millis() - heater_on_timer) / 1000;
      output += " of ";
      output += HEATER_ON_DURATION / 1000;
      output += " seconds\n";
    }
    Serial.println(output);
    lastReport = millis();
  }
}

void heaterOn()
{
  heater_on_times = 0;
  heater_on_timer = millis();
  digitalWrite(HEATER_RELAIS, LOW);
  state = HEATER_ON;
}

void heaterOff()
{
  heater_off_times = 0;
  digitalWrite(HEATER_RELAIS, HIGH);
  state = HEATER_OFF;
}
