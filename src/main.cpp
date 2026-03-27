#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <U8g2lib.h>
#include <MFRC522.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <cstring>
#include <cstdlib>

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

// ---------- WiFi + Server ----------
const char *WIFI_SSID = "labfundamental_2.4GHz";
const char *WIFI_PASSWORD = "labfund1234";
const char *SERVER_RFID_URL = "http://192.168.1.128:8080/rfid"; // Change to your Go server IP
const char *SERVER_REGISTER_START_URL = "http://192.168.1.128:8080/register/start";
const char *SERVER_REGISTER_ENROLL_URL = "http://192.168.1.128:8080/register/enroll";
const unsigned long WIFI_CONNECT_TIMEOUT_MS = 30000;

// ---------- App Data ----------
enum AppState
{
  ST_IDLE,
  ST_REGISTER,
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
char displayUserId[24] = "---";

// Menu
const char *optionNames[3] = {"A", "B", "C"};
const int optionCosts[3] = {10, 20, 30};

// Button tracking
bool lastCancel = HIGH;
bool lastCycle = HIGH;
bool lastConfirm = HIGH;
unsigned long lastWiFiRetryMs = 0;
unsigned long waitRfidMsgUntilMs = 0;

// ---------- Helpers ----------
void setState(AppState newState)
{
  state = newState;
  stateEnterMs = millis();
}

const char *wifiStatusText(wl_status_t st)
{
  switch (st)
  {
  case WL_IDLE_STATUS:
    return "IDLE";
  case WL_NO_SSID_AVAIL:
    return "NO_SSID_AVAIL";
  case WL_SCAN_COMPLETED:
    return "SCAN_COMPLETED";
  case WL_CONNECTED:
    return "CONNECTED";
  case WL_CONNECT_FAILED:
    return "CONNECT_FAILED";
  case WL_CONNECTION_LOST:
    return "CONNECTION_LOST";
  case WL_DISCONNECTED:
    return "DISCONNECTED";
  default:
    return "UNKNOWN";
  }
}

const char *authModeText(wifi_auth_mode_t mode)
{
  switch (mode)
  {
  case WIFI_AUTH_OPEN:
    return "OPEN";
  case WIFI_AUTH_WEP:
    return "WEP";
  case WIFI_AUTH_WPA_PSK:
    return "WPA_PSK";
  case WIFI_AUTH_WPA2_PSK:
    return "WPA2_PSK";
  case WIFI_AUTH_WPA_WPA2_PSK:
    return "WPA_WPA2_PSK";
  case WIFI_AUTH_WPA2_ENTERPRISE:
    return "WPA2_ENTERPRISE";
  case WIFI_AUTH_WPA3_PSK:
    return "WPA3_PSK";
  case WIFI_AUTH_WPA2_WPA3_PSK:
    return "WPA2_WPA3_PSK";
  default:
    return "UNKNOWN";
  }
}

void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info)
{
  if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED)
  {
    Serial.printf("WiFi disconnected, reason=%d\n", info.wifi_sta_disconnected.reason);
  }
  if (event == ARDUINO_EVENT_WIFI_STA_CONNECTED)
  {
    Serial.println("WiFi STA connected to AP");
  }
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
  mockCredit = 0;
  selectedIndex = 0;
  setState(ST_IDLE);
}

bool readAnyRFID()
{
  if (!mfrc522.PICC_IsNewCardPresent())
    return false;
  if (!mfrc522.PICC_ReadCardSerial())
    return false;

  // Convert RFID UID to HEX text (example: "A1B2C3D4")
  char uidBuf[24] = {0};
  for (byte i = 0; i < mfrc522.uid.size && (2 * i + 1) < sizeof(uidBuf); i++)
  {
    snprintf(uidBuf + (2 * i), sizeof(uidBuf) - (2 * i), "%02X", mfrc522.uid.uidByte[i]);
  }

  strncpy(displayUserId, uidBuf, sizeof(displayUserId) - 1);
  displayUserId[sizeof(displayUserId) - 1] = '\0';
  hasCard = true;

  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();
  return true;
}

void showWaitRfidMessage(unsigned long durationMs = 2000)
{
  waitRfidMsgUntilMs = millis() + durationMs;
}

void drawWiFiLoading(uint8_t frame)
{
  static const char spinner[4] = {'|', '/', '-', '\\'};

  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_tr);
  u8g2.drawStr(8, 22, "CONNECTING WIFI");

  char line1[24];
  snprintf(line1, sizeof(line1), "PLEASE WAIT %c", spinner[frame % 4]);
  u8g2.drawStr(16, 38, line1);

  char ssidLine[40];
  snprintf(ssidLine, sizeof(ssidLine), "SSID: %s", WIFI_SSID);
  u8g2.drawStr(0, 58, ssidLine);
  u8g2.sendBuffer();
}

void connectWiFi()
{
  Serial.printf("Connecting WiFi SSID: %s\n", WIFI_SSID);
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);

  int found = 0;
  int targetChannel = 0;
  uint8_t targetBssid[6] = {0};
  bool haveBssid = false;
  int n = WiFi.scanNetworks();
  for (int i = 0; i < n; i++)
  {
    if (WiFi.SSID(i) == WIFI_SSID)
    {
      found++;
      Serial.printf("Found SSID '%s' RSSI=%d dBm channel=%d auth=%s\n",
                    WIFI_SSID, WiFi.RSSI(i), WiFi.channel(i), authModeText((wifi_auth_mode_t)WiFi.encryptionType(i)));
      if (!haveBssid)
      {
        targetChannel = WiFi.channel(i);
        const uint8_t *b = WiFi.BSSID(i);
        if (b != nullptr)
        {
          memcpy(targetBssid, b, 6);
          haveBssid = true;
        }
      }
    }
  }
  if (!found)
  {
    Serial.printf("SSID '%s' not found in scan result.\n", WIFI_SSID);
  }

  WiFi.disconnect(false, true);
  delay(250);

  if (haveBssid && targetChannel > 0)
  {
    Serial.printf("Connecting with channel lock ch=%d bssid=%02X:%02X:%02X:%02X:%02X:%02X\n",
                  targetChannel, targetBssid[0], targetBssid[1], targetBssid[2], targetBssid[3], targetBssid[4], targetBssid[5]);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD, targetChannel, targetBssid, true);
  }
  else
  {
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }

  unsigned long start = millis();
  unsigned long lastPrint = 0;
  uint8_t frame = 0;
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS)
  {
    delay(250);
    drawWiFiLoading(frame++);
    if (millis() - lastPrint >= 1000)
    {
      lastPrint = millis();
      wl_status_t st = WiFi.status();
      Serial.printf("WiFi status: %s (%d)\n", wifiStatusText(st), (int)st);
    }
  }

  if (WiFi.status() == WL_CONNECTED)
  {
    Serial.printf("\nWiFi connected. IP: %s\n", WiFi.localIP().toString().c_str());
  }
  else
  {
    Serial.println("\nWiFi connect timeout. RFID ping will retry when card is scanned.");
  }
}

bool extractJsonInt(const String &json, const char *key, int &valueOut)
{
  String token = String("\"") + key + "\":";
  int keyPos = json.indexOf(token);
  if (keyPos < 0)
  {
    return false;
  }

  int pos = keyPos + token.length();
  while (pos < json.length() && (json[pos] == ' ' || json[pos] == '\t'))
  {
    pos++;
  }

  int end = pos;
  if (end < json.length() && (json[end] == '-' || (json[end] >= '0' && json[end] <= '9')))
  {
    end++;
    while (end < json.length() && (json[end] >= '0' && json[end] <= '9'))
    {
      end++;
    }
  }
  else
  {
    return false;
  }

  String num = json.substring(pos, end);
  valueOut = atoi(num.c_str());
  return true;
}

bool extractJsonBool(const String &json, const char *key, bool &valueOut)
{
  String token = String("\"") + key + "\":";
  int keyPos = json.indexOf(token);
  if (keyPos < 0)
  {
    return false;
  }

  int pos = keyPos + token.length();
  while (pos < json.length() && (json[pos] == ' ' || json[pos] == '\t'))
  {
    pos++;
  }

  if (json.startsWith("true", pos))
  {
    valueOut = true;
    return true;
  }
  if (json.startsWith("false", pos))
  {
    valueOut = false;
    return true;
  }

  return false;
}

bool sendRFIDPing(const char *uid, bool &registeredOut, int &serverCreditOut)
{
  if (WiFi.status() != WL_CONNECTED)
  {
    connectWiFi();
  }

  if (WiFi.status() != WL_CONNECTED)
  {
    return false;
  }

  HTTPClient http;
  http.begin(SERVER_RFID_URL);
  http.addHeader("Content-Type", "application/json");

  char body[96];
  snprintf(body, sizeof(body), "{\"uid\":\"%s\",\"device\":\"esp32-rfid\"}", uid);

  int code = http.POST((uint8_t *)body, strlen(body));
  String response = http.getString();
  http.end();

  Serial.printf("RFID ping -> code=%d, body=%s\n", code, response.c_str());

  if (code > 0 && code < 300)
  {
    bool parsedRegistered = false;
    int parsedCredit = 0;
    bool hasRegistered = extractJsonBool(response, "registered", parsedRegistered);
    bool hasCredit = extractJsonInt(response, "credit", parsedCredit);

    if (hasRegistered)
    {
      registeredOut = parsedRegistered;
      serverCreditOut = hasCredit ? parsedCredit : 0;
      return true;
    }
  }

  return false;
}

bool sendRegisterStartPing()
{
  if (WiFi.status() != WL_CONNECTED)
  {
    connectWiFi();
  }
  if (WiFi.status() != WL_CONNECTED)
  {
    return false;
  }

  HTTPClient http;
  http.begin(SERVER_REGISTER_START_URL);
  http.addHeader("Content-Type", "application/json");

  const char *body = "{\"device\":\"esp32-rfid\"}";
  int code = http.POST((uint8_t *)body, strlen(body));
  String response = http.getString();
  http.end();

  Serial.printf("Register start -> code=%d, body=%s\n", code, response.c_str());
  return code > 0 && code < 300;
}

bool sendRegisterEnrollPing(const char *uid)
{
  if (WiFi.status() != WL_CONNECTED)
  {
    connectWiFi();
  }
  if (WiFi.status() != WL_CONNECTED)
  {
    return false;
  }

  HTTPClient http;
  http.begin(SERVER_REGISTER_ENROLL_URL);
  http.addHeader("Content-Type", "application/json");

  char body[96];
  snprintf(body, sizeof(body), "{\"uid\":\"%s\",\"device\":\"esp32-rfid\"}", uid);

  int code = http.POST((uint8_t *)body, strlen(body));
  String response = http.getString();
  http.end();

  Serial.printf("Register enroll -> code=%d, body=%s\n", code, response.c_str());
  return code > 0 && code < 300;
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
  u8g2.drawStr(16, 24, "BLUE: START");
  u8g2.drawStr(16, 38, "RED : REGISTER");
  u8g2.drawStr(8, 56, "RED clears all IDs");

  u8g2.sendBuffer();
}

void drawRegister()
{
  u8g2.clearBuffer();
  drawHeader();

  u8g2.setFont(u8g2_font_6x12_tr);
  u8g2.drawStr(14, 24, "REGISTER MODE");
  u8g2.drawStr(8, 40, "SCAN RFID TO ADD");
  u8g2.drawStr(6, 56, "BLUE: back  RED: clear");

  u8g2.sendBuffer();
}

void drawWaitRFID()
{
  u8g2.clearBuffer();
  drawHeader();

  u8g2.setFont(u8g2_font_6x12_tr);
  if (millis() < waitRfidMsgUntilMs)
  {
    u8g2.drawStr(8, 24, "PLEASE USE");
    u8g2.drawStr(8, 40, "REGISTERED CARD");
    u8g2.drawStr(16, 58, "Try another card");
    u8g2.sendBuffer();
    return;
  }

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
  WiFi.onEvent(onWiFiEvent);

  // Buttons
  pinMode(BTN_CANCEL, INPUT_PULLUP);
  pinMode(BTN_CYCLE, INPUT_PULLUP);
  pinMode(BTN_CONFIRM, INPUT_PULLUP);

  // Buzzer
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, HIGH);

  // OLED
  Wire.begin(SDA_PIN, SCL_PIN);
  u8g2.begin();

  // RC522 SPI
  SPI.begin(RC522_SCK, RC522_MISO, RC522_MOSI, RC522_SS);
  mfrc522.PCD_Init(RC522_SS, RC522_RST);

  connectWiFi();

  resetToIdle();
  beepQuiet(1, 15, 10);
}

void loop()
{
  if (WiFi.status() != WL_CONNECTED && millis() - lastWiFiRetryMs >= 10000)
  {
    lastWiFiRetryMs = millis();
    connectWiFi();
  }

  bool cancelPressed = pressedEdge(BTN_CANCEL, lastCancel);
  bool cyclePressed = pressedEdge(BTN_CYCLE, lastCycle);
  bool confirmPressed = pressedEdge(BTN_CONFIRM, lastConfirm);

  switch (state)
  {
  case ST_IDLE:
    drawIdle();

    if (cancelPressed)
    {
      bool ok = sendRegisterStartPing();
      Serial.printf("Register mode enter, clear_all=%s\n", ok ? "OK" : "FAIL");
      beepQuiet(ok ? 2 : 1, ok ? 15 : 80, 20);
      setState(ST_REGISTER);
    }

    if (confirmPressed)
    {
      beepQuiet(1, 15, 10);
      setState(ST_WAIT_RFID);
    }
    break;

  case ST_REGISTER:
    drawRegister();

    if (confirmPressed)
    {
      beepQuiet(1, 15, 10);
      resetToIdle();
      break;
    }

    if (cancelPressed)
    {
      bool ok = sendRegisterStartPing();
      Serial.printf("Register clear_all=%s\n", ok ? "OK" : "FAIL");
      beepQuiet(ok ? 2 : 1, ok ? 12 : 80, 18);
    }

    if (readAnyRFID())
    {
      bool ok = sendRegisterEnrollPing(displayUserId);
      Serial.printf("Register enroll uid=%s result=%s\n", displayUserId, ok ? "OK" : "FAIL");
      beepQuiet(ok ? 2 : 1, ok ? 15 : 80, 18);
    }
    break;

  case ST_WAIT_RFID:
    drawWaitRFID();

    if (cancelPressed)
    {
      beepQuiet(2, 12, 18);
      resetToIdle();
      break;
    }

    if (readAnyRFID())
    {
      bool registered = false;
      int serverCredit = mockCredit;
      bool pingOk = sendRFIDPing(displayUserId, registered, serverCredit);
      if (pingOk && registered)
      {
        mockCredit = serverCredit;
        Serial.printf("RFID verified uid=%s credit=%d\n", displayUserId, mockCredit);
        beepQuiet(2, 15, 18);
        setState(ST_MENU);
      }
      else if (pingOk && !registered)
      {
        Serial.printf("RFID rejected uid=%s reason=NOT_REGISTERED\n", displayUserId);
        beepQuiet(1, 80, 20);
        showWaitRfidMessage(2000);
      }
      else
      {
        Serial.printf("RFID processed uid=%s server_ping=FAIL\n", displayUserId);
        beepQuiet(1, 80, 20);
      }
    }

    if (millis() - stateEnterMs >= RFID_TIMEOUT_MS)
    {
      resetToIdle();
    }
    break;

  case ST_MENU:
    drawMenu();

    if (cancelPressed)
    {
      beepQuiet(2, 12, 18);
      resetToIdle();
      break;
    }

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

    if (cancelPressed)
    {
      beepQuiet(2, 12, 18);
      resetToIdle();
      break;
    }

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

    if (cancelPressed)
    {
      beepQuiet(2, 12, 18);
      resetToIdle();
      break;
    }

    // Demo: auto return after 5 sec
    if (millis() - runningStartMs >= 5000)
    {
      resetToIdle();
    }
    break;
  }

  delay(20);
}
