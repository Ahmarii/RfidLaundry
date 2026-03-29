#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <U8g2lib.h>
#include <MFRC522.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <HTTPClient.h>
#include <PubSubClient.h>
#include <cstring>
#include <cstdlib>

// ---------- OLED ----------
#define SDA_PIN 42
#define SCL_PIN 41
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0);

// ---------- INA219 Debug (I2C on separate bus) ----------
#define INA_DEBUG_SDA_PIN 16
#define INA_DEBUG_SCL_PIN 17
TwoWire inaWire(1);
constexpr uint8_t INA219_I2C_ADDR = 0x40;
constexpr float INA_SHUNT_RESISTOR_OHMS = 0.1f; // Adjust to your actual shunt value
unsigned long lastInaDebugMs = 0;
uint8_t inaReadFailStreak = 0;

// ---------- Input ----------
// Update these three pins to match your rotary wiring.
#define ROTARY_DT_PIN 21
#define ROTARY_CLK_PIN 20
#define ROTARY_SW_PIN 12
constexpr bool ROTARY_INVERT_DIRECTION = false;
constexpr int ROTARY_TRANSITIONS_PER_STEP = 4; // reduce sensitivity to avoid skipping options

// ---------- Buzzer ----------
#define BUZZER_PIN 48

// ---------- RC522 ----------
#define RC522_SS 38
#define RC522_SCK 37
#define RC522_MOSI 36
#define RC522_MISO 35
#define RC522_RST 45

// ---------- Motor ----------
#define INA 4
#define INB 5

#define INC 3
#define IND 8

#define EN1 6
#define EN2 15
constexpr uint8_t MOTOR_PWM_CHANNEL = 0;
constexpr uint32_t MOTOR_PWM_FREQ = 20000;
constexpr uint8_t MOTOR_PWM_RESOLUTION = 8;

MFRC522 mfrc522(RC522_SS, RC522_RST);

// ---------- WiFi + Server ----------
const char *WIFI_SSID = "North Room-2.4G.";
const char *WIFI_PASSWORD = "paopao55";
const char *API_BASE_URL = "http://192.168.1.53:8080";
const char *MQTT_BROKER_HOST = "192.168.1.53";
constexpr uint16_t MQTT_BROKER_PORT = 1883;
const char *MQTT_INA219_TOPIC = "esp32/ina219";
const unsigned long WIFI_CONNECT_TIMEOUT_MS = 30000;

// ---------- App Data ----------
enum AppState
{
  ST_WAIT_RFID,
  ST_SELECT,
  ST_READY,
  ST_RUNNING,
  ST_PAUSED,
  ST_FINISH_BEEP
};

AppState state = ST_WAIT_RFID;

const unsigned long PROCESS_DURATION_MS = 10000;
unsigned long stateEnterMs = 0;
unsigned long runningStartMs = 0;
unsigned long runningRemainMs = PROCESS_DURATION_MS;

int mockCredit = 50;
int selectedIndex = 0;
bool hasCard = false;
char displayUserId[24] = "---";

// Menu
const char *optionNames[3] = {"A", "B", "C"};
const int optionCosts[3] = {10, 20, 30};
const int optionSpeeds[3] = {140, 200, 255};
constexpr int MOTOR_MIN_START_PWM = 180;

// Input tracking
int lastSwReading = HIGH;
int stableSwState = HIGH;
unsigned long lastSwDebounceMs = 0;
const unsigned long swDebounceDelayMs = 30;
bool rotarySwPressedEvent = false;

int menuIndex = 0;      // 0..2 => A/B/C, 3 => Cancel
int pauseMenuIndex = 0; // 0 => Resume, 1 => Cancel
unsigned long lastMenuStepMs = 0;
constexpr unsigned long MENU_STEP_COOLDOWN_MS = 60;
unsigned long lastWiFiRetryMs = 0;
unsigned long waitRfidMsgUntilMs = 0;
unsigned long lastMqttRetryMs = 0;

volatile int32_t isrRotaryDelta = 0;
volatile uint8_t isrLastRotaryAB = 0;
volatile int8_t isrRotaryTransitionAcc = 0;
portMUX_TYPE rotaryMux = portMUX_INITIALIZER_UNLOCKED;

bool uiDirty = true;
unsigned long lastUiDrawMs = 0;
constexpr unsigned long UI_IDLE_REDRAW_MS = 400;
constexpr unsigned long UI_RUNNING_REDRAW_MS = 120;

bool beepActive = false;
bool beepOn = false;
int beepPulsesRemain = 0;
int beepOnMs = 20;
int beepOffMs = 25;
unsigned long beepNextToggleMs = 0;

WiFiClient mqttWiFiClient;
PubSubClient mqttClient(mqttWiFiClient);

// ---------- Helpers ----------
void setState(AppState newState)
{
  state = newState;
  stateEnterMs = millis();
  uiDirty = true;
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

bool mqttEnsureConnected()
{
  if (WiFi.status() != WL_CONNECTED)
  {
    return false;
  }

  if (mqttClient.connected())
  {
    return true;
  }

  if (millis() - lastMqttRetryMs < 2000)
  {
    return false;
  }
  lastMqttRetryMs = millis();

  String clientId = String("esp32-client-") + String((uint32_t)ESP.getEfuseMac(), HEX);
  Serial.printf("MQTT connect %s:%u clientId=%s\n", MQTT_BROKER_HOST, MQTT_BROKER_PORT, clientId.c_str());
  if (mqttClient.connect(clientId.c_str()))
  {
    Serial.println("MQTT connected");
    return true;
  }

  Serial.printf("MQTT connect failed rc=%d\n", mqttClient.state());
  return false;
}

void mqttPublishIna219(float busVoltageV, float currentA, float powerW)
{
  if (!mqttEnsureConnected())
  {
    return;
  }

  float currentMa = currentA * 1000.0f;
  float shuntVoltageMv = currentA * INA_SHUNT_RESISTOR_OHMS * 1000.0f;
  float loadVoltageV = busVoltageV + (shuntVoltageMv / 1000.0f);

  char payload[192];
  snprintf(payload, sizeof(payload),
           "{\"bus_voltage_v\":%.3f,\"shunt_voltage_mv\":%.3f,\"load_voltage_v\":%.3f,\"current_ma\":%.3f,\"power_w\":%.3f}",
           busVoltageV, shuntVoltageMv, loadVoltageV, currentMa, powerW);

  if (mqttClient.publish(MQTT_INA219_TOPIC, payload))
  {
    Serial.printf("MQTT pub %s: %s\n", MQTT_INA219_TOPIC, payload);
  }
  else
  {
    Serial.println("MQTT INA219 publish failed");
  }
}

void IRAM_ATTR onRotaryEdgeISR()
{
  portENTER_CRITICAL_ISR(&rotaryMux);

  const uint8_t clk = digitalRead(ROTARY_CLK_PIN) ? 1 : 0;
  const uint8_t dt = digitalRead(ROTARY_DT_PIN) ? 1 : 0;
  const uint8_t ab = (clk << 1) | dt;

  static const int8_t table[16] = {
      0, -1, 1, 0,
      1, 0, 0, -1,
      -1, 0, 0, 1,
      0, 1, -1, 0};

  const int8_t inc = table[(isrLastRotaryAB << 2) | ab];
  isrLastRotaryAB = ab;
  if (inc == 0)
  {
    portEXIT_CRITICAL_ISR(&rotaryMux);
    return;
  }

  isrRotaryTransitionAcc += inc;
  if (isrRotaryTransitionAcc >= ROTARY_TRANSITIONS_PER_STEP)
  {
    isrRotaryDelta += ROTARY_INVERT_DIRECTION ? -1 : 1;
    isrRotaryTransitionAcc = 0;
  }
  else if (isrRotaryTransitionAcc <= -ROTARY_TRANSITIONS_PER_STEP)
  {
    isrRotaryDelta += ROTARY_INVERT_DIRECTION ? 1 : -1;
    isrRotaryTransitionAcc = 0;
  }

  portEXIT_CRITICAL_ISR(&rotaryMux);
}

void updateRotarySwitch()
{
  const int swReading = digitalRead(ROTARY_SW_PIN);
  if (swReading != lastSwReading)
  {
    lastSwDebounceMs = millis();
  }
  if ((millis() - lastSwDebounceMs) > swDebounceDelayMs && swReading != stableSwState)
  {
    stableSwState = swReading;
    if (stableSwState == LOW)
    {
      rotarySwPressedEvent = true;
    }
  }
  lastSwReading = swReading;
}

int consumeRotaryDelta()
{
  portENTER_CRITICAL(&rotaryMux);
  int d = (int)isrRotaryDelta;
  isrRotaryDelta = 0;
  portEXIT_CRITICAL(&rotaryMux);
  return d;
}

bool consumeRotarySwPressed()
{
  bool pressed = rotarySwPressedEvent;
  rotarySwPressedEvent = false;
  return pressed;
}

void updateBeeper()
{
  if (!beepActive)
  {
    return;
  }

  unsigned long now = millis();
  if (now < beepNextToggleMs)
  {
    return;
  }

  if (!beepOn)
  {
    digitalWrite(BUZZER_PIN, LOW);
    beepOn = true;
    beepNextToggleMs = now + (unsigned long)beepOnMs;
  }
  else
  {
    digitalWrite(BUZZER_PIN, HIGH);
    beepOn = false;
    beepPulsesRemain--;
    if (beepPulsesRemain <= 0)
    {
      beepActive = false;
      return;
    }
    beepNextToggleMs = now + (unsigned long)beepOffMs;
  }
}

bool inaReadReg16(uint8_t addr, uint8_t reg, uint16_t &out)
{
  inaWire.beginTransmission(addr);
  inaWire.write(reg);
  // Use STOP between write/read for better robustness under electrical noise.
  if (inaWire.endTransmission(true) != 0)
  {
    return false;
  }

  if (inaWire.requestFrom((int)addr, 2) != 2)
  {
    return false;
  }

  uint8_t msb = inaWire.read();
  uint8_t lsb = inaWire.read();
  out = (uint16_t(msb) << 8) | lsb;
  return true;
}

int16_t asSigned16(uint16_t raw)
{
  return (raw & 0x8000) ? (int16_t)(raw - 0x10000) : (int16_t)raw;
}

void initIna219Bus()
{
  inaWire.begin(INA_DEBUG_SDA_PIN, INA_DEBUG_SCL_PIN, 50000); // 50kHz for better noise margin near motor
  inaWire.setTimeOut(20);
  Serial.printf("INA219 I2C init SDA=%d SCL=%d addr=0x%02X\n", INA_DEBUG_SDA_PIN, INA_DEBUG_SCL_PIN, INA219_I2C_ADDR);
}

void debugInaToSerial()
{
  if (millis() - lastInaDebugMs < 1000)
  {
    return;
  }
  lastInaDebugMs = millis();

  // First, check address ACK once to avoid spamming read errors on a dead bus.
  inaWire.beginTransmission(INA219_I2C_ADDR);
  if (inaWire.endTransmission(true) != 0)
  {
    inaReadFailStreak++;
    Serial.printf("INA219 dbg: no ACK (streak=%u)\n", inaReadFailStreak);
    if (inaReadFailStreak >= 3)
    {
      Serial.println("INA219 dbg: reinitializing I2C bus");
      initIna219Bus();
      inaReadFailStreak = 0;
    }
    return;
  }

  uint16_t reg1 = 0;
  uint16_t reg2 = 0;
  bool ok1 = inaReadReg16(INA219_I2C_ADDR, 0x01, reg1);
  bool ok2 = inaReadReg16(INA219_I2C_ADDR, 0x02, reg2);

  bool anyOk = ok1 || ok2;
  if (!anyOk)
  {
    inaReadFailStreak++;
    Serial.printf("INA219 dbg: read failed (streak=%u)\n", inaReadFailStreak);
    if (inaReadFailStreak >= 3)
    {
      Serial.println("INA219 dbg: reinitializing I2C bus");
      initIna219Bus();
      inaReadFailStreak = 0;
    }
    return;
  }

  inaReadFailStreak = 0;
  Serial.printf("INA219 dbg addr=0x%02X", INA219_I2C_ADDR);
  if (ok1)
    Serial.printf(" R1=%04X", reg1);
  if (ok2)
    Serial.printf(" R2=%04X", reg2);

  // INA219-style estimation:
  // - Shunt voltage reg (0x01): signed, 10 uV/bit
  // - Bus voltage reg (0x02): bits [15:3], 4 mV/bit
  if (ok1 && INA_SHUNT_RESISTOR_OHMS > 0.0f)
  {
    int16_t shuntRaw = asSigned16(reg1);
    float shuntVoltV = (float)shuntRaw * 0.00001f;
    float currentA = shuntVoltV / INA_SHUNT_RESISTOR_OHMS;
    Serial.printf(" I=%.3fA (%.1fmA)", currentA, currentA * 1000.0f);
    if (ok2)
    {
      float busVoltV = ((reg2 >> 3) * 0.004f);
      float powerW = busVoltV * currentA;
      Serial.printf(" P=%.3fW", powerW);
      mqttPublishIna219(busVoltV, currentA, powerW);
    }
  }
  if (ok2)
  {
    float busVoltV = ((reg2 >> 3) * 0.004f);
    Serial.printf(" Vbus=%.3fV", busVoltV);
  }
  Serial.println();
}

void beepQuiet(int pulses = 1, int onMs = 20, int offMs = 25)
{
  if (pulses <= 0)
  {
    return;
  }
  beepPulsesRemain = pulses;
  beepOnMs = onMs;
  beepOffMs = offMs;
  beepOn = false;
  beepActive = true;
  beepNextToggleMs = 0;
}

void stopMotor()
{
  ledcWrite(MOTOR_PWM_CHANNEL, 0);
  digitalWrite(INA, LOW);
  digitalWrite(INB, LOW);
  digitalWrite(INC, LOW);
  digitalWrite(IND, LOW);
}

void runMotorForSelection(int index)
{
  if (index < 0 || index >= 3)
  {
    Serial.printf("Invalid option index=%d\n", index);
    return;
  }

  // Prevent low PWM selections from failing to start under load.
  int speed = optionSpeeds[index];
  if (speed < MOTOR_MIN_START_PWM)
  {
    speed = MOTOR_MIN_START_PWM;
  }

  digitalWrite(INA, HIGH);
  digitalWrite(INB, LOW);
  digitalWrite(INC, HIGH);
  digitalWrite(IND, LOW);
  ledcWrite(MOTOR_PWM_CHANNEL, speed);
  Serial.printf("Motor running option=%s index=%d pwm=%d\n", optionNames[index], index, speed);
}

void resetToIdle()
{
  stopMotor();
  hasCard = false;
  strcpy(displayUserId, "---");
  mockCredit = 0;
  selectedIndex = 0;
  menuIndex = 0;
  pauseMenuIndex = 0;
  runningRemainMs = PROCESS_DURATION_MS;
  setState(ST_WAIT_RFID);
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
  uiDirty = true;
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
  registeredOut = false;
  serverCreditOut = 0;

  if (WiFi.status() != WL_CONNECTED)
  {
    connectWiFi();
  }

  if (WiFi.status() != WL_CONNECTED)
  {
    return false;
  }

  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(5000);
  String url = String(API_BASE_URL) + "/users/" + uid + "/amount";
  Serial.printf("RFID verify: GET %s\n", url.c_str());
  http.begin(url);
  int code = http.GET();
  String response = http.getString();
  http.end();

  if (code <= 0)
  {
    Serial.printf("RFID verify -> code=%d err=%s\n", code, HTTPClient::errorToString(code).c_str());
    return false;
  }

  Serial.printf("RFID verify -> code=%d, body=%s\n", code, response.c_str());

  if (code == 404)
  {
    registeredOut = false;
    return true;
  }

  if (code >= 200 && code < 300)
  {
    int parsedAmount = 0;
    bool hasAmount = extractJsonInt(response, "amount", parsedAmount);
    bool hasCredit = extractJsonInt(response, "credit", parsedAmount); // backward compatibility
    if (hasAmount || hasCredit)
    {
      registeredOut = true;
      serverCreditOut = parsedAmount;
      return true;
    }
  }

  return false;
}

bool updateUserAmountByRFID(const char *uid, int deltaAmount)
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
  http.setConnectTimeout(5000);
  http.setTimeout(5000);
  String url = String(API_BASE_URL) + "/users/" + uid + "/amount";
  Serial.printf("Update amount: PATCH %s\n", url.c_str());
  http.begin(url);
  http.addHeader("Content-Type", "application/json");

  char body[64];
  snprintf(body, sizeof(body), "{\"amount\":%d}", deltaAmount);
  int code = http.sendRequest("PATCH", (uint8_t *)body, strlen(body));
  String response = http.getString();
  http.end();

  if (code <= 0)
  {
    Serial.printf("Update amount -> code=%d err=%s\n", code, HTTPClient::errorToString(code).c_str());
    return false;
  }
  Serial.printf("Update amount -> code=%d, body=%s\n", code, response.c_str());
  return code >= 200 && code < 300;
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

void drawHoverItem(int yTop, const char *text, bool selected)
{
  const int x = 2;
  const int w = 124;
  const int h = 10;
  if (selected)
  {
    u8g2.drawBox(x, yTop, w, h);   // invert row background
    u8g2.drawFrame(x, yTop, w, h); // white border
    u8g2.setDrawColor(0);          // black text on white background
    u8g2.drawStr(x + 4, yTop + 8, text);
    u8g2.setDrawColor(1);
  }
  else
  {
    u8g2.drawFrame(x, yTop, w, h);
    u8g2.drawStr(x + 4, yTop + 8, text);
  }
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
  u8g2.drawStr(8, 42, "Waiting for user...");
  u8g2.drawStr(8, 58, "Use registered card");

  u8g2.sendBuffer();
}

void drawMenu()
{
  u8g2.clearBuffer();
  drawHeader();

  u8g2.setFont(u8g2_font_5x8_tr);
  u8g2.drawStr(2, 20, "Select Option");

  char lineA[24];
  char lineB[24];
  char lineC[24];
  snprintf(lineA, sizeof(lineA), "A - %d", optionCosts[0]);
  snprintf(lineB, sizeof(lineB), "B - %d", optionCosts[1]);
  snprintf(lineC, sizeof(lineC), "C - %d", optionCosts[2]);
  drawHoverItem(22, lineA, menuIndex == 0);
  drawHoverItem(32, lineB, menuIndex == 1);
  drawHoverItem(42, lineC, menuIndex == 2);
  drawHoverItem(52, "Cancel", menuIndex == 3);

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
    u8g2.drawStr(12, 52, "Press knob to start");
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
  u8g2.drawStr(34, 38, line);

  unsigned long elapsedMs = PROCESS_DURATION_MS - runningRemainMs + (millis() - runningStartMs);
  unsigned long sec = elapsedMs / 1000;
  char tbuf[32];
  snprintf(tbuf, sizeof(tbuf), "Time: %lus", sec);
  u8g2.drawStr(6, 54, tbuf);
  u8g2.drawStr(6, 64, "Press knob to pause");

  u8g2.sendBuffer();
}

void drawPaused()
{
  u8g2.clearBuffer();
  drawHeader();
  u8g2.setFont(u8g2_font_5x8_tr);
  u8g2.drawStr(36, 22, "PAUSED");

  char line[32];
  snprintf(line, sizeof(line), "Item %s", optionNames[selectedIndex]);
  u8g2.drawStr(30, 34, line);
  drawHoverItem(40, "Resume", pauseMenuIndex == 0);
  drawHoverItem(50, "Cancel", pauseMenuIndex == 1);
  u8g2.sendBuffer();
}

void setup()
{
  Serial.begin(115200);
  WiFi.onEvent(onWiFiEvent);

  // Inputs
  pinMode(ROTARY_DT_PIN, INPUT_PULLUP);
  pinMode(ROTARY_CLK_PIN, INPUT_PULLUP);
  pinMode(ROTARY_SW_PIN, INPUT_PULLUP);
  isrLastRotaryAB = ((digitalRead(ROTARY_CLK_PIN) ? 1 : 0) << 1) | (digitalRead(ROTARY_DT_PIN) ? 1 : 0);
  isrRotaryTransitionAcc = 0;
  isrRotaryDelta = 0;
  attachInterrupt(digitalPinToInterrupt(ROTARY_CLK_PIN), onRotaryEdgeISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ROTARY_DT_PIN), onRotaryEdgeISR, CHANGE);
  lastSwReading = digitalRead(ROTARY_SW_PIN);
  stableSwState = lastSwReading;

  // Buzzer
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, HIGH);

  // Motor
  pinMode(INA, OUTPUT);
  pinMode(INB, OUTPUT);
  pinMode(INC, OUTPUT);
  pinMode(IND, OUTPUT);
  ledcSetup(MOTOR_PWM_CHANNEL, MOTOR_PWM_FREQ, MOTOR_PWM_RESOLUTION);
  ledcAttachPin(EN1, MOTOR_PWM_CHANNEL);
  ledcAttachPin(EN2, MOTOR_PWM_CHANNEL);
  stopMotor();

  // digitalWrite(INA, HIGH);
  // digitalWrite(INB, LOW);
  // ledcWrite(MOTOR_PWM_CHANNEL, 255);
  // while(1);

  // OLED
  Wire.begin(SDA_PIN, SCL_PIN);
  u8g2.begin();

  // INA219 debug bus (kept separate from OLED I2C bus)
  initIna219Bus();

  // RC522 SPI
  SPI.begin(RC522_SCK, RC522_MISO, RC522_MOSI, RC522_SS);
  mfrc522.PCD_Init(RC522_SS, RC522_RST);

  connectWiFi();
  mqttClient.setServer(MQTT_BROKER_HOST, MQTT_BROKER_PORT);

  resetToIdle();
  beepQuiet(1, 15, 10);
}

void loop()
{
  updateBeeper();

  if (state == ST_RUNNING)
  {
    debugInaToSerial();
  }
  else
  {
    lastInaDebugMs = 0;
  }

  if ((state == ST_WAIT_RFID || state == ST_RUNNING) &&
      WiFi.status() != WL_CONNECTED && millis() - lastWiFiRetryMs >= 10000)
  {
    lastWiFiRetryMs = millis();
    connectWiFi();
  }
  if (state == ST_RUNNING || state == ST_WAIT_RFID)
  {
    mqttEnsureConnected();
    mqttClient.loop();
  }

  updateRotarySwitch();
  int rotate = consumeRotaryDelta();
  bool rotaryPressed = consumeRotarySwPressed();

  switch (state)
  {
  case ST_WAIT_RFID:
    if (uiDirty || millis() - lastUiDrawMs >= UI_IDLE_REDRAW_MS)
    {
      drawWaitRFID();
      uiDirty = false;
      lastUiDrawMs = millis();
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
        menuIndex = 0;
        selectedIndex = 0;
        setState(ST_SELECT);
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
    break;

  case ST_SELECT:
    if (uiDirty || millis() - lastUiDrawMs >= UI_IDLE_REDRAW_MS)
    {
      drawMenu();
      uiDirty = false;
      lastUiDrawMs = millis();
    }

    if (rotate != 0 && (millis() - lastMenuStepMs) >= MENU_STEP_COOLDOWN_MS)
    {
      if (rotate > 0)
      {
        menuIndex = (menuIndex + 1) % 4;
      }
      else
      {
        menuIndex = (menuIndex + 3) % 4;
      }
      lastMenuStepMs = millis();
      uiDirty = true;
    }

    if (rotaryPressed)
    {
      if (menuIndex == 3)
      {
        beepQuiet(2, 12, 12);
        resetToIdle();
      }
      else
      {
        selectedIndex = menuIndex;
        beepQuiet(1, 15, 10);
        setState(ST_READY);
      }
    }
    break;

  case ST_READY:
    if (uiDirty || millis() - lastUiDrawMs >= UI_IDLE_REDRAW_MS)
    {
      drawReadyToStart();
      uiDirty = false;
      lastUiDrawMs = millis();
    }

    if (rotaryPressed)
    {
      if (mockCredit >= optionCosts[selectedIndex])
      {
        int debitDelta = -optionCosts[selectedIndex];
        bool debitOk = updateUserAmountByRFID(displayUserId, debitDelta);
        if (!debitOk)
        {
          Serial.printf("Debit failed uid=%s delta=%d\n", displayUserId, debitDelta);
          beepQuiet(1, 80, 30);
          break;
        }

        // Re-sync with backend after debit.
        bool registered = false;
        int serverCredit = mockCredit;
        bool refreshOk = sendRFIDPing(displayUserId, registered, serverCredit);
        if (refreshOk && registered)
        {
          mockCredit = serverCredit;
          uiDirty = true;
        }

        beepQuiet(3, 12, 18);
        runningStartMs = millis();
        runningRemainMs = PROCESS_DURATION_MS;
        runMotorForSelection(selectedIndex);
        setState(ST_RUNNING);
      }
      else
      {
        beepQuiet(1, 80, 30);
        setState(ST_SELECT);
      }
    }
    break;

  case ST_RUNNING:
    if (uiDirty || millis() - lastUiDrawMs >= UI_RUNNING_REDRAW_MS)
    {
      drawRunning();
      uiDirty = false;
      lastUiDrawMs = millis();
    }

    if (rotaryPressed)
    {
      unsigned long seg = millis() - runningStartMs;
      if (seg < runningRemainMs)
      {
        runningRemainMs -= seg;
      }
      else
      {
        runningRemainMs = 0;
      }
      stopMotor();
      beepQuiet(1, 12, 12);
      pauseMenuIndex = 0;
      setState(ST_PAUSED);
      break;
    }

    if (millis() - runningStartMs >= runningRemainMs)
    {
      stopMotor();
      setState(ST_FINISH_BEEP);
    }
    break;

  case ST_PAUSED:
    if (uiDirty || millis() - lastUiDrawMs >= UI_IDLE_REDRAW_MS)
    {
      drawPaused();
      uiDirty = false;
      lastUiDrawMs = millis();
    }
    if (rotate != 0 && (millis() - lastMenuStepMs) >= MENU_STEP_COOLDOWN_MS)
    {
      pauseMenuIndex = (pauseMenuIndex + 1) % 2;
      lastMenuStepMs = millis();
      uiDirty = true;
    }
    if (rotaryPressed)
    {
      if (pauseMenuIndex == 0)
      {
        runningStartMs = millis();
        runMotorForSelection(selectedIndex);
        beepQuiet(1, 12, 12);
        setState(ST_RUNNING);
      }
      else
      {
        beepQuiet(2, 12, 12);
        resetToIdle();
      }
    }
    break;

  case ST_FINISH_BEEP:
    beepQuiet(2, 60, 80);
    resetToIdle();
    break;
  default:
    resetToIdle();
    break;
  }

  // Guard against wrap/negative behavior if timing got out of sync.
  if (state == ST_RUNNING)
  {
    unsigned long seg = millis() - runningStartMs;
    if (seg > runningRemainMs)
    {
      runningRemainMs = 0;
    }
  }

  delay(2);
}
