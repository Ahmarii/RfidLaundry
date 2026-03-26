#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <U8g2lib.h>
#include <MFRC522.h>

// ---------- OLED ----------
#define SDA_PIN 42
#define SCL_PIN 41
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0);

// ---------- Buttons ----------
#define BTN_CANCEL 20  // red
#define BTN_CYCLE 21   // white
#define BTN_CONFIRM 47 // blue

// ---------- Buzzer ----------
#define BUZZER_PIN 48

// ---------- RC522 ----------
#define RC522_SS 38
#define RC522_SCK 37
#define RC522_MOSI 36
#define RC522_MISO 35
#define RC522_RST 45

MFRC522 mfrc522(RC522_SS, RC522_RST);

// ---------- App Data ----------
enum AppState
{
  ST_IDLE,
  ST_WAIT_RFID,
  ST_MENU,
  ST_READY_TO_START,
  ST_RUNNING
};

AppState state = ST_IDLE;

const unsigned long RFID_TIMEOUT_MS = 30000;
unsigned long stateEnterMs = 0;
unsigned long runningStartMs = 0;

int mockCredit = 50;
int selectedIndex = 0;
bool hasCard = false;
char displayUserId[8] = "---";

// Menu
const char *optionNames[3] = {"A", "B", "C"};
const int optionCosts[3] = {10, 20, 30};

// Button tracking
bool lastCancel = HIGH;
bool lastCycle = HIGH;
bool lastConfirm = HIGH;

// ---------- Helpers ----------
void setState(AppState newState)
{
  state = newState;
  stateEnterMs = millis();
}

bool pressedEdge(int pin, bool &lastState)
{
  bool current = digitalRead(pin);
  bool pressed = (lastState == HIGH && current == LOW);
  lastState = current;
  return pressed;
}

void beepQuiet(int pulses = 1, int onMs = 20, int offMs = 25)
{
  for (int i = 0; i < pulses; i++)
  {
    digitalWrite(BUZZER_PIN, LOW);
    delay(onMs);
    digitalWrite(BUZZER_PIN, HIGH);
    delay(offMs);
  }
}

void resetToIdle()
{
  hasCard = false;
  strcpy(displayUserId, "---");
  selectedIndex = 0;
  setState(ST_IDLE);
}

bool readAnyRFID()
{
  if (!mfrc522.PICC_IsNewCardPresent())
    return false;
  if (!mfrc522.PICC_ReadCardSerial())
    return false;

  // For your test flow, any detected card becomes mock user "001"
  strcpy(displayUserId, "001");
  hasCard = true;

  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();
  return true;
}

void drawHeader()
{
  char creditBuf[24];
  snprintf(creditBuf, sizeof(creditBuf), "CR:%d", mockCredit);
  u8g2.setFont(u8g2_font_6x12_tr);
  u8g2.drawStr(0, 10, creditBuf);

  char idBuf[24];
  snprintf(idBuf, sizeof(idBuf), "ID:%s", displayUserId);
  int w = u8g2.getStrWidth(idBuf);
  u8g2.drawStr(128 - w, 10, idBuf);
}

void drawIdle()
{
  u8g2.clearBuffer();
  drawHeader();

  u8g2.setFont(u8g2_font_6x12_tr);
  u8g2.drawStr(20, 28, "PRESS BLUE");
  u8g2.drawStr(28, 42, "TO START");
  u8g2.drawStr(10, 58, "RED=CANCEL anytime");

  u8g2.sendBuffer();
}

void drawWaitRFID()
{
  u8g2.clearBuffer();
  drawHeader();

  u8g2.setFont(u8g2_font_6x12_tr);
  u8g2.drawStr(18, 26, "SCAN RFID CARD");

  unsigned long elapsed = millis() - stateEnterMs;
  unsigned long remain = (elapsed >= RFID_TIMEOUT_MS) ? 0 : (RFID_TIMEOUT_MS - elapsed) / 1000;

  char tbuf[24];
  snprintf(tbuf, sizeof(tbuf), "Timeout: %lus", remain);
  u8g2.drawStr(20, 42, tbuf);
  u8g2.drawStr(18, 58, "RED=Cancel");

  u8g2.sendBuffer();
}

void drawMenu()
{
  u8g2.clearBuffer();
  drawHeader();

  u8g2.setFont(u8g2_font_6x12_tr);
  u8g2.drawStr(0, 24, "Select Option");

  for (int i = 0; i < 3; i++)
  {
    char line[32];
    snprintf(line, sizeof(line), "%c %s - %d", (selectedIndex == i ? '>' : ' '), optionNames[i], optionCosts[i]);
    u8g2.drawStr(8, 38 + i * 10, line);
  }

  u8g2.sendBuffer();
}

void drawReadyToStart()
{
  u8g2.clearBuffer();
  drawHeader();

  u8g2.setFont(u8g2_font_6x12_tr);

  char line1[32];
  snprintf(line1, sizeof(line1), "Choice: %s", optionNames[selectedIndex]);
  u8g2.drawStr(12, 24, line1);

  char line2[32];
  snprintf(line2, sizeof(line2), "Cost: %d", optionCosts[selectedIndex]);
  u8g2.drawStr(12, 38, line2);

  if (mockCredit >= optionCosts[selectedIndex])
  {
    u8g2.drawStr(12, 52, "BLUE to START");
  }
  else
  {
    u8g2.drawStr(12, 52, "Not enough credit");
  }

  u8g2.sendBuffer();
}

void drawRunning()
{
  u8g2.clearBuffer();
  drawHeader();

  u8g2.setFont(u8g2_font_6x12_tr);
  u8g2.drawStr(28, 26, "RUNNING");

  char line[32];
  snprintf(line, sizeof(line), "Item %s", optionNames[selectedIndex]);
  u8g2.drawStr(34, 40, line);

  unsigned long sec = (millis() - runningStartMs) / 1000;
  char tbuf[32];
  snprintf(tbuf, sizeof(tbuf), "Time: %lus", sec);
  u8g2.drawStr(32, 56, tbuf);

  u8g2.sendBuffer();
}

void setup()
{
  Serial.begin(115200);

  // Buttons
  pinMode(BTN_CANCEL, INPUT_PULLUP);
  pinMode(BTN_CYCLE, INPUT_PULLUP);
  pinMode(BTN_CONFIRM, INPUT_PULLUP);

  // Buzzer
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, HIGH0.0);

  // OLED
  Wire.begin(SDA_PIN, SCL_PIN);
  u8g2.begin();

  // RC522 SPI
  SPI.begin(RC522_SCK, RC522_MISO, RC522_MOSI, RC522_SS);
  mfrc522.PCD_Init(RC522_SS, RC522_RST);

  resetToIdle();
  beepQuiet(1, 15, 10);
}

void loop()
{
  bool cancelPressed = pressedEdge(BTN_CANCEL, lastCancel);
  bool cyclePressed = pressedEdge(BTN_CYCLE, lastCycle);
  bool confirmPressed = pressedEdge(BTN_CONFIRM, lastConfirm);

  // Cancel from almost anywhere
  if (cancelPressed)
  {
    beepQuiet(2, 12, 18);
    resetToIdle();
  }

  switch (state)
  {
  case ST_IDLE:
    drawIdle();

    if (confirmPressed)
    {
      beepQuiet(1, 15, 10);
      setState(ST_WAIT_RFID);
    }
    break;

  case ST_WAIT_RFID:
    drawWaitRFID();

    if (readAnyRFID())
    {
      beepQuiet(2, 15, 18);
      setState(ST_MENU);
    }

    if (millis() - stateEnterMs >= RFID_TIMEOUT_MS)
    {
      resetToIdle();
    }
    break;

  case ST_MENU:
    drawMenu();

    if (cyclePressed)
    {
      selectedIndex = (selectedIndex + 1) % 3;
      beepQuiet(1, 10, 8);
    }

    if (confirmPressed)
    {
      beepQuiet(1, 15, 10);
      setState(ST_READY_TO_START);
    }
    break;

  case ST_READY_TO_START:
    drawReadyToStart();

    if (cyclePressed)
    {
      selectedIndex = (selectedIndex + 1) % 3;
      beepQuiet(1, 10, 8);
    }

    if (confirmPressed)
    {
      if (mockCredit >= optionCosts[selectedIndex])
      {
        mockCredit -= optionCosts[selectedIndex];
        beepQuiet(3, 12, 18);
        runningStartMs = millis();
        setState(ST_RUNNING);
      }
      else
      {
        beepQuiet(1, 80, 30);
      }
    }
    break;

  case ST_RUNNING:
    drawRunning();

    // Demo: auto return after 5 sec
    if (millis() - runningStartMs >= 5000)
    {
      resetToIdle();
    }
    break;
  }

  delay(20);
}
