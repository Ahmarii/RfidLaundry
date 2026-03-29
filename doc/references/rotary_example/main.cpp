#include <Arduino.h>

const int pinCLK = 27;
const int pinDT = 26;
const int pinSW = 25;

int lastCLK = HIGH;
int value = 0;
unsigned long lastEncEdgeMs = 0;
const unsigned long encoderDebounceMs = 2;
unsigned long lastDiagMs = 0;
const unsigned long diagIntervalMs = 500;

int lastSwReading = HIGH;
int stableSwState = HIGH;
unsigned long lastDebounceMs = 0;
const unsigned long debounceDelayMs = 30;

void setup()
{
  Serial.begin(115200);
  pinMode(pinCLK, INPUT_PULLUP);
  pinMode(pinDT, INPUT_PULLUP);
  pinMode(pinSW, INPUT_PULLUP);

  lastCLK = digitalRead(pinCLK);
  lastSwReading = digitalRead(pinSW);
  stableSwState = lastSwReading;

  Serial.println("Rotary encoder ready.");
}

void loop()
{
  const int clkState = digitalRead(pinCLK);

  // Count one step on each falling edge of CLK.
  if (clkState != lastCLK && (millis() - lastEncEdgeMs) > encoderDebounceMs)
  {
    lastEncEdgeMs = millis();
    if (digitalRead(pinDT) != clkState)
    {
      value++; // Clockwise
      Serial.print("CW  -> value: ");
    }
    else
    {
      value--; // Counter-clockwise
      Serial.print("CCW -> value: ");
    }
    Serial.println(value);
  }
  lastCLK = clkState;

  if ((millis() - lastDiagMs) > diagIntervalMs)
  {
    lastDiagMs = millis();
    Serial.print("RAW CLK=");
    Serial.print(digitalRead(pinCLK));
    Serial.print(" DT=");
    Serial.print(digitalRead(pinDT));
    Serial.print(" SW=");
    Serial.print(digitalRead(pinSW));
    Serial.print(" value=");
    Serial.println(value);
  }

  // Non-blocking debounce for the push button.
  const int swReading = digitalRead(pinSW);
  if (swReading != lastSwReading)
  {
    lastDebounceMs = millis();
  }

  if ((millis() - lastDebounceMs) > debounceDelayMs && swReading != stableSwState)
  {
    stableSwState = swReading;
    if (stableSwState == LOW)
    {
      Serial.println("Button pressed");
    }
  }

  lastSwReading = swReading;
}
