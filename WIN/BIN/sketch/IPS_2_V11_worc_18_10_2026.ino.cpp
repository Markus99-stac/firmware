#include <Arduino.h>
#line 1 "E:\\Downloads\\IPS_2_V11_worc_18_10_2026\\IPS_2_V11_worc_18_10_2026.ino"
// ================================================================
//  IPS v2.0 — ESP32-S3
// ================================================================

#include <Wire.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <ADS1115_WE.h>
#include <Adafruit_SHT4x.h>
// #include <MAX17055.h>          // заменено заглушкой ниже
#include <diod5.h>
#include <diod3.h>
#include <Preferences.h>
#include <NimBLEDevice.h>

#include "FontsRus/FreeMonoBoldOblique18.h"   // FreeMono18

// ================================================================
// ЗАГЛУШКА MAX17055 (вместо <MAX17055.h>)
// ================================================================
class MAX17055Stub {
public:
  bool  begin()            { return true; }
  float getVoltageCell()   { return 3.90f; }   // В
  float getCurrent_mA()    { return 0.0f;  }   // мА
  float getRepSOC()        { return 75.0f; }   // %
  float getTemp()          { return 25.0f; }   // °C
};
MAX17055Stub MAX17055;

// ========== ПИНЫ ==========
#define TFT_MOSI 11
#define TFT_MISO 12
#define TFT_CLK  13
#define TFT_RST  14
#define TFT_DC   1
#define TFT_CS   10
#define TFT_LED  20
#define PWEN_5V  18

#define I2C_SDA 8
#define I2C_SCL 9

#define BTN_10_100V   16
#define BTN_TEST      15
#define VSW           17

#define RELAY_10K      4
#define RELAY_1M       5
#define RELAY_1G       6
#define RELAY_10G      7

#define ADS1115_ADDR  0x48
#define MAX17055_ADDR 0x36

// ========== ТЕКСТ ==========
#define string1 20
#define string2 48
#define font &FreeMonoBoldOblique18pt8b

// ========== МЕНЮ / ЯРКОСТЬ ==========
Preferences prefs;
bool    inMenu        = false;
uint8_t brightness    = 200;
const uint8_t BRIGHT_MIN = 10;
const uint8_t BRIGHT_MAX = 255;

// ========== ОТЛОЖЕННОЕ СОХРАНЕНИЕ ЯРКОСТИ ==========
bool          brightnessDirty   = false;
unsigned long brightnessDirtyAt = 0;
const unsigned long BRIGHT_SAVE_DELAY_MS = 1500;

// для детектора двойного зажатия
static bool          comboActive       = false;
static unsigned long comboStart        = 0;
static bool          comboTriggered    = false;
const unsigned long  COMBO_HOLD_MS     = 800;

// ========== COM-КОМАНДЫ РЕЖИМА ЯРКОСТИ ==========
#define COM_CMD_BRIGHT_ENTER  "BRIGHT:ENTER"
#define COM_CMD_BRIGHT_EXIT   "BRIGHT:EXIT"
#define COM_CMD_BRIGHT_VALUE  "BRIGHT:%d"

static unsigned long menuEnteredAt = 0;
const unsigned long  MENU_AUTO_EXIT_MS = 5000;

// ================================================================
// ========== BLE-UART (Nordic UART Service) ======================
// ================================================================
#define BLE_DEVICE_NAME "IPS_Device"

static const char* NUS_SERVICE_UUID = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
static const char* NUS_RX_CHAR_UUID = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E";
static const char* NUS_TX_CHAR_UUID = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E";

class BleUart {
public:
  NimBLEServer*         pServer = nullptr;
  NimBLECharacteristic* pTxChar = nullptr;
  NimBLECharacteristic* pRxChar = nullptr;
  volatile bool         connected = false;

  void begin(const char* name) {
    NimBLEDevice::init(name);
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);

    pServer = NimBLEDevice::createServer();
    pServer->setCallbacks(new ServerCB(this));

    NimBLEService* svc = pServer->createService(NUS_SERVICE_UUID);

    pTxChar = svc->createCharacteristic(
        NUS_TX_CHAR_UUID,
        NIMBLE_PROPERTY::NOTIFY);

    pRxChar = svc->createCharacteristic(
        NUS_RX_CHAR_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    pRxChar->setCallbacks(new RxCB(this));

    svc->start();

    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID(NUS_SERVICE_UUID);
    adv->setName(name);
    adv->start();

    Serial.printf("[BLE] started as \"%s\"\n", name);
  }

  bool hasClient() { return connected; }

  void println(const char* s) {
    if (!connected || !pTxChar) return;
    pTxChar->setValue((const uint8_t*)s, strlen(s));
    pTxChar->notify();
    const uint8_t nl[2] = {'\r', '\n'};
    pTxChar->setValue(nl, 2);
    pTxChar->notify();
  }

  void println() {
    if (!connected || !pTxChar) return;
    const uint8_t nl[2] = {'\r', '\n'};
    pTxChar->setValue(nl, 2);
    pTxChar->notify();
  }

  void printfln(const char* fmt, ...) {
    if (!connected || !pTxChar) return;
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    pTxChar->setValue((const uint8_t*)buf, strlen(buf));
    pTxChar->notify();
    const uint8_t nl[2] = {'\r', '\n'};
    pTxChar->setValue(nl, 2);
    pTxChar->notify();
  }

  void print(const char* s) {
    if (!connected || !pTxChar) return;
    pTxChar->setValue((const uint8_t*)s, strlen(s));
    pTxChar->notify();
  }

  bool hasCommand() { return _cmdReady; }

  void pollCommand(String& out) {
    if (!_cmdReady) return;
    out = _cmdBuf;
    _cmdBuf = "";
    _cmdReady = false;
  }

private:
  String        _cmdBuf;
  volatile bool _cmdReady = false;

  struct ServerCB : public NimBLEServerCallbacks {
    BleUart* self;
    ServerCB(BleUart* s) : self(s) {}
    void onConnect(NimBLEServer* s, NimBLEConnInfo& info) override {
      self->connected = true;
      Serial.println("[BLE] client connected");
      NimBLEDevice::getAdvertising()->start();
    }
    void onDisconnect(NimBLEServer* s, NimBLEConnInfo& info, int reason) override {
      self->connected = false;
      Serial.printf("[BLE] client disconnected (reason=%d)\n", reason);
      NimBLEDevice::getAdvertising()->start();
    }
  };

  struct RxCB : public NimBLECharacteristicCallbacks {
    BleUart* self;
    RxCB(BleUart* s) : self(s) {}
    void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& info) override {
      std::string v = c->getValue();
      if (v.empty()) return;
      for (char ch : v) {
        if (ch == '\n' || ch == '\r') {
          if (self->_cmdBuf.length() > 0) {
            self->_cmdBuf.trim();
            self->_cmdBuf.toLowerCase();
            self->_cmdReady = true;
          }
        } else {
          if (self->_cmdBuf.length() < 120) self->_cmdBuf += ch;
        }
      }
    }
  };
};

BleUart SerialBT;

// ========== ОБЪЕКТЫ ==========
Adafruit_ILI9341 tft = Adafruit_ILI9341(TFT_CS, TFT_DC, TFT_RST);
Adafruit_SHT4x sht40 = Adafruit_SHT4x();
ADS1115_WE adc = ADS1115_WE(ADS1115_ADDR);

// ========== ПРОТОТИПЫ ==========
void drawMainScreen();
void updateDisplayInfo();
void drawBatteryIndicator();
void showBootScreen();
void showResult();
void drawProgressBar(int x, int y, int w, int h, int percent, String text);
void performMeasurement();
void perform10VMeasurement();
void perform100VMeasurement();
void handleSerialCommands();
void handleBleCommands();
void handleCommand(String cmd, bool fromBle);
void handleButtons();
void readSingleChannel(String channel);
void readAllChannels();
void printHelp();
void initMAX17055();
void updateBatteryData();
void readTemperatureHumidity();
void initDisplay();
void initSHT40();
void formatResistanceValue(float value);
void allRelaysOff();
unsigned long _poligon(unsigned int value, unsigned long in[], unsigned long out[], int size);
char *utf8rus2(char *source);

void drawMenuScreen();
void drawBrightnessBar();
void applyBrightness();
void handleMenuButtons();
void loadBrightness();
void saveBrightness();
void drawCenterHint();

void drawLogo(int cx, int cy);
void drawModeIndicator();
void drawCenterText(const char* text);
void drawBottomBar();

void comSendBrightnessMode(bool enter);
void comSendBrightnessValue();

void markBrightnessDirty();

// ========== ПОЛИГОННЫЕ ТАБЛИЦЫ ==========
unsigned long _Poligon_InArray_2[26]  = {13, 174, 230, 343, 379, 484, 672, 834, 1099, 1618, 3064, 3360, 4183, 5544, 8203, 15734, 16487, 18236, 19258, 20398, 21681, 23138, 24801, 26720, 27791, 28956};
unsigned long _Poligon_OutArray_2[26] = {1000000, 200000, 150000, 100000, 90000, 70000, 50000, 40000, 30000, 20000, 10000, 9000, 7000, 5000, 3000, 1000, 900, 700, 600, 500, 400, 300, 200, 100, 50, 1};

unsigned long _Poligon_InArray_3[26]  = {30, 49, 103, 318, 349, 450, 628, 1032, 2885, 3234, 4039, 5378, 8058, 16054, 16870, 18841, 21330, 24571, 28979, 29779, 30341, 30923, 31526, 31679, 31806, 31834};
unsigned long _Poligon_OutArray_3[26] = {700000, 500000, 300000, 100000, 90000, 70000, 50000, 30000, 10000, 9000, 7000, 5000, 3000, 1000, 900, 700, 500, 300, 100, 70, 50, 30, 10, 5, 1, 0};

unsigned long _Poligon_InArray_0[25]  = {2769, 2785, 2790, 2816, 2827, 3019, 3236, 3583, 4086, 5964, 5988, 6630, 7775, 9788, 16300, 17146, 19113, 21281, 24158, 28107, 28795, 29295, 29829, 30383, 30616};
unsigned long _Poligon_OutArray_0[25] = {1000000, 900000, 700000, 500000, 300000, 100000, 50000, 30000, 20000, 10000, 9000, 7000, 5000, 3000, 1000, 900, 700, 500, 300, 100, 70, 50, 30, 10, 0};

unsigned long _Poligon_InArray_1[8]  = {25475, 25483, 25500, 25522, 25595, 25690, 25990, 28112};
unsigned long _Poligon_OutArray_1[8] = {1000, 900, 700, 500, 300, 100, 50, 0};

// ========== ПЕРЕМЕННЫЕ ==========
float _gtvA0, _gtvA1, _gtvA2, _gtvA3;
float _gtvA0F, _gtvA1F, _gtvA2F, _gtvA3F;
float _gtv0, _gtv1, _gtv2, _gtv3;

float Rx = 0;
String _gtv8 = "0";
bool is10VMode = true;
bool measurementComplete = false;
bool measurementInProgress = false;
float temperature = 0, humidity = 0;
String measurementInfo = "";
unsigned long lastSensorRead = 0;

float bat_voltage = 0;
float bat_current = 0;
float bat_soc = 0;
float bat_temp = 0;
bool max17055_ok = false;

float prevBatVoltage = -1;
float prevBatSoc = -1;
float prevTemperature = -999;
float prevHumidity = -999;

#define maxString 100
char target[maxString + 1] = "";

// ================================================================
// COM (Serial) — команды режима яркости
// ================================================================
#line 342 "E:\\Downloads\\IPS_2_V11_worc_18_10_2026\\IPS_2_V11_worc_18_10_2026.ino"
void setup();
#line 392 "E:\\Downloads\\IPS_2_V11_worc_18_10_2026\\IPS_2_V11_worc_18_10_2026.ino"
void loop();
#line 315 "E:\\Downloads\\IPS_2_V11_worc_18_10_2026\\IPS_2_V11_worc_18_10_2026.ino"
void comSendBrightnessMode(bool enter) {
  if (enter) {
    Serial.println(COM_CMD_BRIGHT_ENTER);
    Serial.printf(COM_CMD_BRIGHT_VALUE "\n", brightness);
  } else {
    Serial.println(COM_CMD_BRIGHT_EXIT);
  }
  if (SerialBT.hasClient()) {
    if (enter) {
      SerialBT.println(COM_CMD_BRIGHT_ENTER);
      SerialBT.printfln(COM_CMD_BRIGHT_VALUE, brightness);
    } else {
      SerialBT.println(COM_CMD_BRIGHT_EXIT);
    }
  }
}

void comSendBrightnessValue() {
  Serial.printf(COM_CMD_BRIGHT_VALUE "\n", brightness);
  if (SerialBT.hasClient()) {
    SerialBT.printfln(COM_CMD_BRIGHT_VALUE, brightness);
  }
}

// ================================================================
// SETUP
// ================================================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== IPS v2.0 ===");

  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000);

  initMAX17055();

  pinMode(VSW, OUTPUT); digitalWrite(VSW, LOW);
  pinMode(RELAY_10K, OUTPUT); digitalWrite(RELAY_10K, LOW);
  pinMode(RELAY_1M,  OUTPUT); digitalWrite(RELAY_1M,  LOW);
  pinMode(RELAY_1G,  OUTPUT); digitalWrite(RELAY_1G,  LOW);
  pinMode(RELAY_10G, OUTPUT); digitalWrite(RELAY_10G, LOW);
  pinMode(PWEN_5V, OUTPUT);  digitalWrite(PWEN_5V, HIGH);

  pinMode(BTN_10_100V, INPUT_PULLUP);
  pinMode(BTN_TEST,    INPUT_PULLUP);

  loadBrightness();
  ledcAttach(TFT_LED, 5000, 8);
  applyBrightness();

  initDisplay();
  showBootScreen();

  SerialBT.begin(BLE_DEVICE_NAME);
  SerialBT.println("IPS Device started");

  initSHT40();

  if (!adc.init()) {
    SerialBT.println("ERROR: ADS1115!");
  } else {
    SerialBT.println("ADS1115 OK");
    adc.setConvRate(ADS1115_860_SPS);
    adc.setMeasureMode(ADS1115_CONTINUOUS);
  }

  drawMainScreen();
  printHelp();

  Serial.printf("=== ГОТОВ === brightness=%d\n", brightness);
  SerialBT.printfln("=== ГОТОВ === brightness=%d", brightness);
}

// ================================================================
// loop()
// ================================================================
void loop() {
  unsigned long currentMillis = millis();

  // авто-выход из меню яркости через 5 с
  if (inMenu && (millis() - menuEnteredAt >= MENU_AUTO_EXIT_MS)) {
    inMenu = false;
    comSendBrightnessMode(false);      // COM: выход из режима яркости
    saveBrightness();
    brightnessDirty = false;           // уже сохранили — сбрасываем флаг
    drawMainScreen();
    Serial.println("[MENU] auto-exit (5s)");
    if (SerialBT.hasClient()) SerialBT.println("[MENU] auto-exit (5s)");
  }

  // отложенное сохранение яркости (через 1.5 с после последнего изменения)
  if (brightnessDirty && (millis() - brightnessDirtyAt >= BRIGHT_SAVE_DELAY_MS)) {
    saveBrightness();
    brightnessDirty = false;
    Serial.printf("[BRIGHT] saved %d\n", brightness);
    if (SerialBT.hasClient()) SerialBT.printfln("[BRIGHT] saved %d", brightness);
  }

  handleSerialCommands();
  handleBleCommands();

  static unsigned long lastButtonCheck = 0;
  if (currentMillis - lastButtonCheck >= 50) {
    lastButtonCheck = currentMillis;
    if (inMenu) handleMenuButtons();
    else        handleButtons();
  }

  if (currentMillis - lastSensorRead >= 2000 && !measurementInProgress) {
    lastSensorRead = currentMillis;
    readTemperatureHumidity();
    updateBatteryData();

    if (max17055_ok &&
        (abs(bat_voltage - prevBatVoltage) > 0.05 ||
         abs(bat_soc - prevBatSoc) > 1.0 ||
         abs(temperature - prevTemperature) > 0.1 ||
         abs(humidity - prevHumidity) > 0.5)) {
      updateDisplayInfo();
      prevBatVoltage = bat_voltage;
      prevBatSoc = bat_soc;
      prevTemperature = temperature;
      prevHumidity = humidity;
    }

    if (SerialBT.hasClient()) {
      char btBuffer[256];
      if (max17055_ok) {
        snprintf(btBuffer, sizeof(btBuffer),
                 "T:%.1f,H:%.0f,B:%.2fV,SOC:%.1f%%,M:%s,Rx:%.2f%s",
                 temperature, humidity, bat_voltage, bat_soc,
                 is10VMode ? "10V" : "100V",
                 Rx, measurementInfo.c_str());
      } else {
        snprintf(btBuffer, sizeof(btBuffer),
                 "T:%.1f,H:%.0f,B:ERR,M:%s,Rx:%.2f%s",
                 temperature, humidity,
                 is10VMode ? "10V" : "100V",
                 Rx, measurementInfo.c_str());
      }
      SerialBT.println(btBuffer);
    }
  }

  delay(10);
}

// ================================================================
// Команды Serial
// ================================================================
void handleSerialCommands() {
  if (!Serial.available()) return;
  String cmd = Serial.readStringUntil('\n');
  if (cmd.length() == 0) return;
  Serial.println("\n[CMD] " + cmd);
  handleCommand(cmd, false);
}

// ================================================================
// Команды BLE
// ================================================================
void handleBleCommands() {
  if (!SerialBT.hasCommand()) return;
  String cmd;
  SerialBT.pollCommand(cmd);
  if (cmd.length() == 0) return;
  Serial.println("[BLE CMD] " + cmd);
  handleCommand(cmd, true);
}

// ================================================================
//  обработчик команд
// ================================================================
void handleCommand(String cmd, bool fromBle) {
  cmd.trim();
  cmd.toLowerCase();

  auto reply = [&](const char* s) {
    Serial.println(s);
    if (fromBle) SerialBT.println(s);
  };
  auto replyf = [&](const char* fmt, ...) {
    char buf[256];
    va_list args; va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    Serial.println(buf);
    if (fromBle) SerialBT.println(buf);
  };

  if (cmd == "relay10k on" || cmd == "r10k on" || cmd == "r10k+") {
    digitalWrite(RELAY_10K, HIGH); reply("RELAY_10K ON");
  }
  else if (cmd == "relay10k off" || cmd == "r10k off" || cmd == "r10k-") {
    digitalWrite(RELAY_10K, LOW); reply("RELAY_10K OFF");
  }
  else if (cmd == "relay1m on" || cmd == "r1m on" || cmd == "r1m+") {
    digitalWrite(RELAY_1M, HIGH); reply("RELAY_1M ON");
  }
  else if (cmd == "relay1m off" || cmd == "r1m off" || cmd == "r1m-") {
    digitalWrite(RELAY_1M, LOW); reply("RELAY_1M OFF");
  }
  else if (cmd == "relay1g on" || cmd == "r1g on" || cmd == "r1g+") {
    digitalWrite(RELAY_1G, HIGH); reply("RELAY_1G ON");
  }
  else if (cmd == "relay1g off" || cmd == "r1g off" || cmd == "r1g-") {
    digitalWrite(RELAY_1G, LOW); reply("RELAY_1G OFF");
  }
  else if (cmd == "relay10g on" || cmd == "r10g on" || cmd == "r10g+") {
    digitalWrite(RELAY_10G, HIGH); reply("RELAY_10G ON");
  }
  else if (cmd == "relay10g off" || cmd == "r10g off" || cmd == "r10g-") {
    digitalWrite(RELAY_10G, LOW); reply("RELAY_10G OFF");
  }
  else if (cmd == "all off" || cmd == "alloff" || cmd == "off all") {
    allRelaysOff(); reply("ALL RELAYS OFF");
  }
  else if (cmd == "all on" || cmd == "allon" || cmd == "on all") {
    digitalWrite(RELAY_10K, HIGH);
    digitalWrite(RELAY_1M,  HIGH);
    digitalWrite(RELAY_1G,  HIGH);
    digitalWrite(RELAY_10G, HIGH);
    reply("ALL RELAYS ON");
  }
  else if (cmd == "vsw on" || cmd == "vsw+") {
    is10VMode = false;
    digitalWrite(VSW, HIGH);
    drawModeIndicator();
    measurementComplete = false;
    reply("VSW ON (100V mode)");
  }
  else if (cmd == "vsw off" || cmd == "vsw-") {
    is10VMode = true;
    digitalWrite(VSW, LOW);
    drawModeIndicator();
    measurementComplete = false;
    reply("VSW OFF (10V mode)");
  }
  else if (cmd == "status" || cmd == "stat" || cmd == "st") {
    replyf("RELAY_10K: %s", digitalRead(RELAY_10K) ? "ON" : "OFF");
    replyf("RELAY_1M:  %s", digitalRead(RELAY_1M)  ? "ON" : "OFF");
    replyf("RELAY_1G:  %s", digitalRead(RELAY_1G)  ? "ON" : "OFF");
    replyf("RELAY_10G: %s", digitalRead(RELAY_10G) ? "ON" : "OFF");
    replyf("VSW:       %s", digitalRead(VSW) ? "ON (100V)" : "OFF (10V)");
    replyf("Mode:      %s", is10VMode ? "10V" : "100V");
    replyf("Brightness:%d", brightness);
  }
  else if (cmd.startsWith("read ")) {
    String channel = cmd.substring(5);
    readSingleChannel(channel);
  }
  else if (cmd == "read" || cmd == "r") {
    readAllChannels();
  }
  else if (cmd == "test" || cmd == "measure") {
    performMeasurement();
  }
  else if (cmd == "help" || cmd == "h" || cmd == "?") {
    printHelp();
  }
  else if (cmd == "bright enter" || cmd == "bright:enter") {
    if (!inMenu) {
      inMenu = true;
      menuEnteredAt = millis();
      comSendBrightnessMode(true);
      drawMenuScreen();
      reply("[MENU] entered via COM");
    } else {
      reply("[MENU] already active");
    }
  }
  else if (cmd == "bright exit" || cmd == "bright:exit") {
    if (inMenu) {
      inMenu = false;
      comSendBrightnessMode(false);
      saveBrightness();
      brightnessDirty = false;
      drawMainScreen();
      reply("[MENU] exited via COM");
    } else {
      reply("[MENU] not active");
    }
  }
  else if (cmd.startsWith("bright set ")) {
    int val = cmd.substring(11).toInt();
    if (val >= BRIGHT_MIN && val <= BRIGHT_MAX) {
      brightness = (uint8_t)val;
      menuEnteredAt = millis();       // продлеваем сессию
      applyBrightness();
      markBrightnessDirty();
      if (inMenu) drawBrightnessBar();
      comSendBrightnessValue();
      replyf("Brightness set to %d", brightness);
    } else {
      replyf("Invalid value (range %d..%d)", BRIGHT_MIN, BRIGHT_MAX);
    }
  }
  else if (cmd == "bright +") {
    brightness = (brightness + 25 > BRIGHT_MAX) ? BRIGHT_MAX : brightness + 25;
    menuEnteredAt = millis();
    applyBrightness();
    markBrightnessDirty();
    if (inMenu) drawBrightnessBar();
    comSendBrightnessValue();
    replyf("Brightness = %d", brightness);
  }
  else if (cmd == "bright -") {
    brightness = (brightness < BRIGHT_MIN + 25) ? BRIGHT_MIN : brightness - 25;
    menuEnteredAt = millis();
    applyBrightness();
    markBrightnessDirty();
    if (inMenu) drawBrightnessBar();
    comSendBrightnessValue();
    replyf("Brightness = %d", brightness);
  }
  else {
    reply("Unknown command. Type 'help'.");
  }
}

// ================================================================
// MAX17055
// ================================================================
void initMAX17055() {
  Wire.beginTransmission(MAX17055_ADDR);
  byte error = Wire.endTransmission();
  if (error == 0) {
    MAX17055.begin();
    max17055_ok = true;
    updateBatteryData();
  } else {
    max17055_ok = false;
  }
}

void updateBatteryData() {
  if (!max17055_ok) return;
  bat_voltage = MAX17055.getVoltageCell();
  bat_current = MAX17055.getCurrent_mA();
  bat_soc     = MAX17055.getRepSOC();
  bat_temp    = MAX17055.getTemp();
}

// ================================================================
// ДИСПЛЕЙ
// ================================================================
void showBootScreen() {
  tft.fillScreen(ILI9341_BLACK);
  tft.setFont(font);

  tft.drawRGBBitmap(110, 25, diod3, 100, 123);

  tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(90, 170);
  tft.print(utf8rus2("IPS v2.0"));

  delay(800);
  tft.setFont();
}

void drawLogo(int cx, int cy) {
  const int W = 25;
  const int H = 25;
  tft.drawRGBBitmap(cx - W/2, cy - H/2, diod5, W, H);
}

void drawModeIndicator() {
  tft.fillRect(0, 10, 130, 30, ILI9341_DARKGREY);
  tft.setFont(font);
  tft.setTextColor(ILI9341_CYAN);
  tft.setCursor(5, string1 + 15);
  tft.print(is10VMode ? "10B" : "100B");
  tft.setFont();
}

void drawCenterText(const char* text) {
  tft.setFont(font);

  int16_t x1, y1;
  uint16_t w, h;
  tft.setTextColor(ILI9341_BLACK);
  tft.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);

  const int16_t PAD = 5;
  int16_t textY = 135;
  int16_t bgY   = textY + y1 - PAD;

  tft.fillRect(0, bgY, tft.width(), h + 2 * PAD, ILI9341_GREEN);

  tft.setTextColor(ILI9341_BLACK);
  int16_t textX = (tft.width() - w) / 2;
  tft.setCursor(textX, textY);
  tft.print(text);

  tft.setFont();
}

void drawBottomBar() {
  tft.fillRect(0, 205, 320, 35, ILI9341_BLACK);
  tft.setFont(font);
  tft.setTextColor(ILI9341_YELLOW);

  tft.setCursor(10, 230);
  tft.printf("T:%.1fC", temperature);

  tft.setCursor(190, 230);
  tft.printf("H:%.0f%%", humidity);

  tft.setFont();
}

void drawProgressBar(int x, int y, int w, int h, int percent, String text) {
  tft.setFont(font);

  tft.fillRect(x - 5, y - 5, w + 10, h + 30, ILI9341_BLACK);
  tft.drawRect(x, y, w, h, ILI9341_WHITE);

  int fillWidth = (w - 2) * percent / 100;
  if (fillWidth > 0) {
    uint16_t color;
    if (percent < 30)      color = ILI9341_RED;
    else if (percent < 70) color = ILI9341_YELLOW;
    else                   color = ILI9341_GREEN;
    tft.fillRect(x + 1, y + 1, fillWidth, h - 2, color);
  }

  tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(x, y + h + 19);
  tft.print(text);

  tft.setFont();
}

void drawMainScreen() {
  tft.fillScreen(ILI9341_BLACK);
  tft.fillRect(0, 10, 320, 30, ILI9341_DARKGREY);

  drawModeIndicator();
  drawLogo(160, 25);
  drawBatteryIndicator();

  tft.drawLine(0, 40, 320, 40, ILI9341_WHITE);

  drawCenterHint();
  drawBottomBar();

  tft.setFont();
}

void drawCenterHint() {
  tft.fillRect(0, 35, 320, 80, ILI9341_BLACK);
  tft.setFont(font);
  tft.setTextColor(ILI9341_RED);

  const char* l1 = utf8rus2("НАЖАТЬ");
  char buf1[64]; strncpy(buf1, l1, sizeof(buf1)-1); buf1[sizeof(buf1)-1] = 0;

  const char* l2 = utf8rus2("КЛАВИШУ ТЕСТ");
  char buf2[64]; strncpy(buf2, l2, sizeof(buf2)-1); buf2[sizeof(buf2)-1] = 0;

  int16_t x1, y1; uint16_t w, h;
  tft.getTextBounds(buf1, 0, 0, &x1, &y1, &w, &h);
  tft.setCursor((tft.width() - w) / 2, 80);
  tft.print(buf1);

  tft.getTextBounds(buf2, 0, 0, &x1, &y1, &w, &h);
  tft.setCursor((tft.width() - w) / 2, 110);
  tft.print(buf2);

  tft.setFont();
}

void drawBatteryIndicator() {
  int x = 180, y = 13;

  tft.fillRect(x - 5, y - 2, 130, 30, ILI9341_DARKGREY);

  int batPercent = 0;
  if (max17055_ok) {
    if (bat_voltage >= 4.05) {
      batPercent = 100;
    } else if (bat_voltage <= 3.0) {
      batPercent = 0;
    } else {
      batPercent = (int)((bat_voltage - 3.0) * 95.24);
    }
    batPercent = constrain(batPercent, 0, 100);
  } else {
    batPercent = 50;
  }

  tft.drawRect(x, y + 2, 45, 16, ILI9341_WHITE);
  tft.fillRect(x + 45, y + 6, 4, 8, ILI9341_WHITE);

  tft.drawRect(x + 1,  y + 3, 13, 14, ILI9341_WHITE);
  tft.drawRect(x + 16, y + 3, 13, 14, ILI9341_WHITE);
  tft.drawRect(x + 31, y + 3, 13, 14, ILI9341_WHITE);

  uint16_t batColor;
  if (batPercent > 66) batColor = ILI9341_GREEN;
  else if (batPercent > 33) batColor = ILI9341_YELLOW;
  else batColor = ILI9341_RED;

  int totalFillWidth = 42 * batPercent / 100;

  int seg1Fill = constrain(totalFillWidth, 0, 13);
  if (seg1Fill > 0) tft.fillRect(x + 2, y + 4, seg1Fill, 12, batColor);

  int seg2Fill = constrain(totalFillWidth - 14, 0, 13);
  if (seg2Fill > 0) tft.fillRect(x + 17, y + 4, seg2Fill, 12, batColor);

  int seg3Fill = constrain(totalFillWidth - 28, 0, 13);
  if (seg3Fill > 0) tft.fillRect(x + 32, y + 4, seg3Fill, 12, batColor);

  tft.fillRect(x + 60, y, 70, 24, ILI9341_DARKGREY);
  tft.setFont(font);
  tft.setTextColor(ILI9341_CYAN);
  tft.setCursor(x + 62, y + 20);
  tft.printf("%d%%", batPercent);
  tft.setFont();
}

void updateDisplayInfo() {
  tft.fillRect(0, 10, 320, 30, ILI9341_DARKGREY);
  drawModeIndicator();
  drawLogo(160, 25);
  drawBatteryIndicator();
  tft.drawLine(0, 40, 320, 40, ILI9341_WHITE);
  drawBottomBar();
  tft.setFont();
}

// ================================================================
// ИЗМЕРЕНИЕ
// ================================================================
void handleButtons() {
  static bool lastTestState = HIGH, lastModeState = HIGH;
  static unsigned long lastTestPress = 0, lastModePress = 0;

  bool testState = digitalRead(BTN_TEST);
  bool modeState = digitalRead(BTN_10_100V);

  bool bothPressed = (testState == LOW) && (modeState == LOW);

  if (bothPressed && !comboActive) {
    comboActive    = true;
    comboStart     = millis();
    comboTriggered = false;
  } else if (bothPressed && comboActive && !comboTriggered) {
    if (millis() - comboStart >= COMBO_HOLD_MS) {
      comboTriggered = true;
      inMenu = true;
      menuEnteredAt = millis();
      comSendBrightnessMode(true);
      lastTestState = testState;
      lastModeState = modeState;
      drawMenuScreen();
      Serial.println("[MENU] entered");
      if (SerialBT.hasClient()) SerialBT.println("[MENU] entered");
      return;
    }
  } else if (!bothPressed) {
    comboActive    = false;
    comboTriggered = false;
  }

  if (testState == LOW && lastTestState == HIGH &&
      (millis() - lastTestPress) > 2000 && !bothPressed) {
    lastTestPress = millis();
    performMeasurement();
  }
  lastTestState = testState;

  if (modeState == LOW && lastModeState == HIGH &&
      (millis() - lastModePress) > 500 && !bothPressed) {
    lastModePress = millis();
    is10VMode = !is10VMode;
    measurementComplete = false;
    drawModeIndicator();
    digitalWrite(VSW, is10VMode ? LOW : HIGH);
    Serial.printf("Mode: %s\n", is10VMode ? "10B" : "100B");
    if (SerialBT.hasClient()) {
      SerialBT.printfln("Mode: %s", is10VMode ? "10B" : "100B");
    }
  }
  lastModeState = modeState;
}

// ================================================================
// МЕНЮ ЯРКОСТИ
// ================================================================
void handleMenuButtons() {
  static bool lastTestState = HIGH, lastModeState = HIGH;
  static unsigned long lastRepeat = 0;

  bool testState = digitalRead(BTN_TEST);
  bool modeState = digitalRead(BTN_10_100V);

  bool bothPressed = (testState == LOW) && (modeState == LOW);

  if (bothPressed && !comboActive) {
    comboActive    = true;
    comboStart     = millis();
    comboTriggered = false;
  } else if (bothPressed && comboActive && !comboTriggered) {
    if (millis() - comboStart >= COMBO_HOLD_MS) {
      comboTriggered = true;
      inMenu = false;
      comboActive = false;
      comSendBrightnessMode(false);
      lastTestState = testState;
      lastModeState = modeState;
      saveBrightness();
      brightnessDirty = false;
      drawMainScreen();
      Serial.println("[MENU] exited");
      if (SerialBT.hasClient()) SerialBT.println("[MENU] exited");
      return;
    }
  } else if (!bothPressed) {
    comboActive    = false;
    comboTriggered = false;
  }

  if (testState == LOW && lastTestState == HIGH) {
    brightness = (brightness + 25 > BRIGHT_MAX) ? BRIGHT_MAX : brightness + 25;
    menuEnteredAt = millis();
    applyBrightness();
    markBrightnessDirty();
    drawBrightnessBar();
    comSendBrightnessValue();
    lastRepeat = millis();
  } else if (testState == LOW && (millis() - lastRepeat) > 200) {
    brightness = (brightness + 5 > BRIGHT_MAX) ? BRIGHT_MAX : brightness + 5;
    menuEnteredAt = millis();
    applyBrightness();
    markBrightnessDirty();
    drawBrightnessBar();
    comSendBrightnessValue();
    lastRepeat = millis();
  }
  lastTestState = testState;

  if (modeState == LOW && lastModeState == HIGH) {
    brightness = (brightness < BRIGHT_MIN + 25) ? BRIGHT_MIN : brightness - 25;
    menuEnteredAt = millis();
    applyBrightness();
    markBrightnessDirty();
    drawBrightnessBar();
    comSendBrightnessValue();
    lastRepeat = millis();
  } else if (modeState == LOW && (millis() - lastRepeat) > 200) {
    brightness = (brightness < BRIGHT_MIN + 5) ? BRIGHT_MIN : brightness - 5;
    menuEnteredAt = millis();
    applyBrightness();
    markBrightnessDirty();
    drawBrightnessBar();
    comSendBrightnessValue();
    lastRepeat = millis();
  }
  lastModeState = modeState;
}

void applyBrightness() {
  ledcWrite(TFT_LED, brightness);
}

void loadBrightness() {
  prefs.begin("ips", true);
  brightness = prefs.getUChar("bright", 200);
  prefs.end();
  brightness = constrain(brightness, BRIGHT_MIN, BRIGHT_MAX);
}

void saveBrightness() {
  prefs.begin("ips", false);
  prefs.putUChar("bright", brightness);
  prefs.end();
}

void markBrightnessDirty() {
  brightnessDirty = true;
  brightnessDirtyAt = millis();
}

void drawBrightnessBar() {
  const int x = 30, y = 110, w = 260, h = 30;
  tft.fillRect(x - 2, y - 2, w + 4, h + 24, ILI9341_BLACK);
  tft.drawRect(x, y, w, h, ILI9341_WHITE);

  int percent = map(brightness, BRIGHT_MIN, BRIGHT_MAX, 0, 100);
  int fillW   = (w - 2) * percent / 100;

  uint16_t color;
  if (percent < 30)      color = ILI9341_RED;
  else if (percent < 70) color = ILI9341_YELLOW;
  else                   color = ILI9341_GREEN;

  if (fillW > 0) tft.fillRect(x + 1, y + 1, fillW, h - 2, color);

  tft.setFont(font);
  tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(x, y + h + 25);
  tft.printf("%d%%", percent);
  tft.setFont();
}

void drawMenuScreen() {
  tft.fillScreen(ILI9341_BLACK);
  tft.fillRect(0, 10, 320, 30, ILI9341_DARKGREY);

  tft.setFont(font);
  tft.setTextColor(ILI9341_CYAN);
  tft.setCursor(20, string1 + 15);
  tft.print(utf8rus2("НАСТРОЙКИ"));
  tft.setFont();

  tft.drawLine(0, 40, 320, 40, ILI9341_WHITE);

  tft.setFont(font);
  tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(10, 90);
  tft.print(utf8rus2("Яркость:"));
  tft.setFont();

  drawBrightnessBar();

  tft.setFont(font);
  tft.setTextColor(ILI9341_YELLOW);
  tft.setCursor(10, 200);
  tft.print(utf8rus2("   10/100=-"));
  tft.setCursor(10, 230);
  tft.print(utf8rus2("   ТЕСТ=+"));
  tft.setFont();
}

void performMeasurement() {
  measurementInProgress = true;
  measurementComplete = false;

  tft.fillRect(0, 41, 320, 160, ILI9341_BLACK);

  Serial.println("\n=== MEASUREMENT ===");
  Serial.printf("Mode: %s\n", is10VMode ? "10V" : "100V");
  Serial.printf("Temperature: %.1f C\n", temperature);
  Serial.printf("Humidity: %.0f %%\n", humidity);

  if (SerialBT.hasClient()) {
    SerialBT.println("=== MEASUREMENT ===");
    SerialBT.printfln("Mode: %s", is10VMode ? "10V" : "100V");
    SerialBT.printfln("Temperature: %.1f C", temperature);
    SerialBT.printfln("Humidity: %.0f %%", humidity);
  }

  drawProgressBar(30, 55, 260, 14, 10, utf8rus2(""));
  allRelaysOff();
  delay(300);

  drawProgressBar(30, 55, 260, 14, 40, utf8rus2(""));
  delay(1000);

  if (is10VMode) {
    perform10VMeasurement();
  } else {
    perform100VMeasurement();
  }

  drawProgressBar(30, 55, 260, 14, 100, utf8rus2(""));
  measurementInProgress = false;
  measurementComplete = true;

  showResult();

  String unit = "";
  if (_gtv8 == "0") unit = " Ohm";
  else if (_gtv8 == "3") unit = " kOhm";
  else if (_gtv8 == "6") unit = " MOhm";
  else if (_gtv8 == "9") unit = " GOhm";

  Serial.println("\n=== REPORT ===");
  Serial.printf("Mode: %s\n", is10VMode ? "10V" : "100V");
  Serial.printf("Temperature: %.1f C\n", temperature);
  Serial.printf("Humidity: %.0f %%\n", humidity);
  Serial.printf("Resistance: %.2f%s %s\n", Rx, unit.c_str(), measurementInfo.c_str());
  Serial.println("=== END REPORT ===\n");

  if (SerialBT.hasClient()) {
    SerialBT.println("\n=== REPORT ===");
    SerialBT.printfln("Mode: %s", is10VMode ? "10V" : "100V");
    SerialBT.printfln("Temperature: %.1f C", temperature);
    SerialBT.printfln("Humidity: %.0f %%", humidity);
    SerialBT.printfln("Resistance: %.2f%s %s", Rx, unit.c_str(), measurementInfo.c_str());
    SerialBT.println("=== END REPORT ===\n");
  }

  delay(5000);
  drawCenterHint();
}

void perform10VMeasurement() {
  drawProgressBar(30, 55, 260, 14, 55, utf8rus2(""));

  digitalWrite(RELAY_10K, HIGH);
  digitalWrite(RELAY_1M, LOW);
  digitalWrite(RELAY_1G, LOW);
  digitalWrite(RELAY_10G, LOW);
  delay(500);

  adc.setVoltageRange_mV(ADS1115_RANGE_2048);
  adc.setCompareChannels(ADS1115_COMP_0_GND);
  delay(50);

  float raw = 0;
  for (int i = 0; i < 128; i++) {
    raw += adc.getRawResult();
    delay(2);
  }
  raw /= 128.0;

  Serial.printf("RAW A0: %.2f\n", raw);

  drawProgressBar(30, 55, 260, 14, 90, utf8rus2(""));

  if (raw < 10) {
    Rx = 0;
    measurementInfo = "(Нет сигнала)";
    Serial.println("Нет сигнала");
  } else {
    float result = _poligon((unsigned int)raw, _Poligon_InArray_2, _Poligon_OutArray_2, 26);
    formatResistanceValue(result);
    measurementInfo = "";
    Serial.printf("Result: %.2f\n", result);
  }

  digitalWrite(RELAY_10K, LOW);
}

void perform100VMeasurement() {
  drawProgressBar(30, 55, 260, 14, 55, utf8rus2(""));

  digitalWrite(RELAY_1M, HIGH);
  digitalWrite(RELAY_1G, LOW);
  digitalWrite(RELAY_10G, LOW);
  delay(500);

  adc.setVoltageRange_mV(ADS1115_RANGE_2048);
  adc.setCompareChannels(ADS1115_COMP_1_GND);
  delay(50);

  float raw1 = 0;
  for (int i = 0; i < 128; i++) {
    raw1 += adc.getRawResult();
    delay(2);
  }
  raw1 /= 128.0;

  Serial.printf("RAW A1 (1M): %.0f\n", raw1);

  if (raw1 > 1000) {
    drawProgressBar(30, 55, 260, 14, 85, utf8rus2(""));
    float result = _poligon((unsigned int)raw1, _Poligon_InArray_3, _Poligon_OutArray_3, 26);
    result = result / 1000.0;
    formatResistanceValue(result);
    measurementInfo = "";
    digitalWrite(RELAY_1M, LOW);
    Serial.printf("Result 1M: %.0f\n", result);
    return;
  }
  digitalWrite(RELAY_1M, LOW);

  drawProgressBar(30, 55, 260, 14, 65, utf8rus2(""));

  digitalWrite(RELAY_1G, HIGH);
  delay(500);

  adc.setVoltageRange_mV(ADS1115_RANGE_2048);
  adc.setCompareChannels(ADS1115_COMP_2_GND);
  delay(50);

  float raw2 = 0;
  for (int i = 0; i < 128; i++) {
    raw2 += adc.getRawResult();
    delay(2);
  }
  raw2 /= 128.0;

  Serial.printf("RAW A2 (1G): %.0f\n", raw2);

  if (raw2 > 1000) {
    drawProgressBar(30, 55, 260, 14, 85, utf8rus2(""));
    float result = _poligon((unsigned int)raw2, _Poligon_InArray_0, _Poligon_OutArray_0, 25);
    formatResistanceValue(result);
    measurementInfo = "";
    digitalWrite(RELAY_1G, LOW);
    Serial.printf("Result 1G: %.0f\n", result);
    return;
  }
  digitalWrite(RELAY_1G, LOW);

  drawProgressBar(30, 55, 260, 14, 75, utf8rus2(""));

  digitalWrite(RELAY_10G, HIGH);
  delay(1000);

  adc.setVoltageRange_mV(ADS1115_RANGE_0256);
  adc.setCompareChannels(ADS1115_COMP_3_GND);
  delay(50);

  float raw_sum = 0;
  int valid = 0;

  for (int i = 0; i < 20; i++) {
    float raw = adc.getRawResult();
    if (i >= 5 && raw > 0 && raw < 32767) {
      raw_sum += raw;
      valid++;
    }
    if (i == 5)  drawProgressBar(30, 55, 260, 14, 78, utf8rus2(""));
    if (i == 10) drawProgressBar(30, 55, 260, 14, 81, utf8rus2(""));
    if (i == 15) drawProgressBar(30, 55, 260, 14, 84, utf8rus2(""));
    delay(100);
  }

  drawProgressBar(30, 55, 260, 14, 90, utf8rus2(""));

  if (valid > 0) {
    float raw_avg = raw_sum / valid;
    Serial.printf("RAW A3 avg (10G): %.0f\n", raw_avg);

    float resistance_mohm;
    if (raw_avg < 15000) {
      resistance_mohm = 13500.0 / raw_avg * 10000.0;
    } else {
      resistance_mohm = raw_avg * 0.000769;
    }

    Serial.printf("resistance_mohm: %.0f\n", resistance_mohm);
    formatResistanceValue(resistance_mohm);
    measurementInfo = "";
  } else {
    Rx = 0;
    measurementInfo = "(no signal 10G)";
  }

  digitalWrite(RELAY_10G, LOW);
}

void showResult() {
  char buf[48];

  if (Rx == 0) {
    drawCenterText("Нет сигнала");
    return;
  }

  double valueOhm = 0.0;
  if      (_gtv8 == "0") valueOhm = Rx;
  else if (_gtv8 == "3") valueOhm = Rx * 1e3;
  else if (_gtv8 == "6") valueOhm = Rx * 1e6;
  else if (_gtv8 == "9") valueOhm = Rx * 1e9;

  int exp10 = 0;
  if (valueOhm >= 1.0) {
    while (valueOhm >= 10.0) { valueOhm /= 10.0; exp10++; }
  } else if (valueOhm > 0.0) {
    while (valueOhm < 1.0) { valueOhm *= 10.0; exp10--; }
  }

  snprintf(buf, sizeof(buf), "%.2f * 10E%d", valueOhm, exp10);
  drawCenterText(buf);
}

// ================================================================
// ЧТЕНИЕ КАНАЛОВ ADS1115
// ================================================================
void readSingleChannel(String channel) {
  channel.toLowerCase();
  int ch = -1;
  const char* ch_name = "";
  int comp = 0;
  ADS1115_RANGE range = ADS1115_RANGE_2048;

  if (channel == "a0" || channel == "0") { ch = 0; ch_name = "A0"; comp = 0; range = ADS1115_RANGE_0256; }
  else if (channel == "a1" || channel == "1") { ch = 1; ch_name = "A1"; comp = 1; range = ADS1115_RANGE_2048; }
  else if (channel == "a2" || channel == "2") { ch = 2; ch_name = "A2"; comp = 2; range = ADS1115_RANGE_2048; }
  else if (channel == "a3" || channel == "3") { ch = 3; ch_name = "A3"; comp = 3; range = ADS1115_RANGE_2048; }
  else return;

  Serial.printf("\n=== CHANNEL %s ===\n", ch_name);
  Serial.printf("Range: %s\n", range == ADS1115_RANGE_0256 ? "+-256mV" : "+-2048mV");

  adc.setVoltageRange_mV(range);
  switch(comp) {
    case 0: adc.setCompareChannels(ADS1115_COMP_0_GND); break;
    case 1: adc.setCompareChannels(ADS1115_COMP_1_GND); break;
    case 2: adc.setCompareChannels(ADS1115_COMP_2_GND); break;
    case 3: adc.setCompareChannels(ADS1115_COMP_3_GND); break;
  }
  delay(50);

  const int samples = 128;
  long sum = 0;
  int valid = 0;

  for (int i = 0; i < samples; i++) {
    int raw = adc.getRawResult();
    if (raw >= 0 && raw <= 32767) { sum += raw; valid++; }
    if (i < 10) {
      Serial.printf("  [%3d] raw=%5d\n", i, raw);
    }
    delay(2);
  }

  float avg = (valid > 0) ? (sum / (float)valid) : 0;
  float voltage = (range == ADS1115_RANGE_0256) ? avg * 0.0000078125 : avg * 0.0000625;

  Serial.printf("Avg RAW: %.2f\n", avg);
  Serial.printf("Voltage: %.3f mV\n", voltage * 1000);
  Serial.println("=== END ===\n");
}

void readAllChannels() {
  Serial.println("\n=== ALL CHANNELS ===");

  int channels[] = {0, 1, 2, 3};
  const char* names[] = {"A0", "A1", "A2", "A3"};
  ADS1115_RANGE ranges[] = {
    ADS1115_RANGE_0256,
    ADS1115_RANGE_2048,
    ADS1115_RANGE_2048,
    ADS1115_RANGE_2048
  };

  for (int ch = 0; ch < 4; ch++) {
    Serial.printf("\n--- CHANNEL %s ---\n", names[ch]);

    adc.setVoltageRange_mV(ranges[ch]);
    switch(ch) {
      case 0: adc.setCompareChannels(ADS1115_COMP_0_GND); break;
      case 1: adc.setCompareChannels(ADS1115_COMP_1_GND); break;
      case 2: adc.setCompareChannels(ADS1115_COMP_2_GND); break;
      case 3: adc.setCompareChannels(ADS1115_COMP_3_GND); break;
    }
    delay(50);

    long sum = 0;
    int valid = 0;
    int minVal = 32767, maxVal = 0;

    for (int i = 0; i < 20; i++) {
      int raw = adc.getRawResult();
      if (raw >= 0 && raw <= 32767) {
        sum += raw;
        valid++;
        if (raw < minVal) minVal = raw;
        if (raw > maxVal) maxVal = raw;
      }
      delay(5);
    }

    float avg = (valid > 0) ? (sum / (float)valid) : 0;
    float voltage = (ranges[ch] == ADS1115_RANGE_0256) ? avg * 0.0000078125 : avg * 0.0000625;

    Serial.printf("Avg RAW: %.2f\n", avg);
    Serial.printf("Min/Max: %d / %d\n", minVal, maxVal);
    Serial.printf("Voltage: %.3f mV\n", voltage * 1000);
  }
  Serial.println("\n=== END ===\n");
}

// ================================================================
// HELP
// ================================================================
void printHelp() {
  const char* lines[] = {
    "=== COMMANDS ===",
    "RELAYS: r10k on/off | r1m on/off | r1g on/off | r10g on/off",
    "        all on/off",
    "POWER:  vsw on/off   (100V / 10V mode)",
    "MEASURE: read a0..a3 | read | test",
    "OTHER:  status | help",
    "BRIGHT: bright enter | bright exit | bright set <10..255> | bright + | bright -",
    "=== END ==="
  };
  for (auto& l : lines) {
    Serial.println(l);
    if (SerialBT.hasClient()) SerialBT.println(l);
  }
}

// ================================================================
// ВСПОМОГАТЕЛЬНЫЕ
// ================================================================
void formatResistanceValue(float value) {
  if (is10VMode) {
    if (value < 1000)          { Rx = value;              _gtv8 = "0"; }
    else if (value < 1000000)  { Rx = value / 1000.0;     _gtv8 = "3"; }
    else                       { Rx = value / 1000000.0;  _gtv8 = "6"; }
  } else {
    if (value < 0.001)         { Rx = value * 1000000;    _gtv8 = "0"; }
    else if (value < 1)        { Rx = value * 1000;       _gtv8 = "3"; }
    else if (value < 1000)     { Rx = value;              _gtv8 = "6"; }
    else                       { Rx = value / 1000.0;     _gtv8 = "9"; }
  }
}

void allRelaysOff() {
  digitalWrite(RELAY_10K, LOW);
  digitalWrite(RELAY_1M, LOW);
  digitalWrite(RELAY_1G, LOW);
  digitalWrite(RELAY_10G, LOW);
}

unsigned long _poligon(unsigned int value, unsigned long in[], unsigned long out[], int size) {
  if (value <= in[0]) return out[0];
  if (value >= in[size-1]) return out[size-1];
  for (int i = 0; i < size-1; i++)
    if (value >= in[i] && value <= in[i+1])
      return map(value, in[i], in[i+1], out[i], out[i+1]);
  return out[size-1];
}

// ================================================================
// ИНИЦИАЛИЗАЦИЯ ПЕРИФЕРИИ
// ================================================================
void initDisplay() {
  pinMode(TFT_RST, OUTPUT);
  digitalWrite(TFT_RST, LOW); delay(10);
  digitalWrite(TFT_RST, HIGH); delay(120);
  SPI.begin(TFT_CLK, TFT_MISO, TFT_MOSI, TFT_CS);
  tft.begin();
  tft.setRotation(3);
}

void initSHT40() {
  if (sht40.begin()) {
    sht40.setPrecision(SHT4X_HIGH_PRECISION);
    sht40.setHeater(SHT4X_NO_HEATER);
  }
}

void readTemperatureHumidity() {
  sensors_event_t hum, temp;
  if (sht40.getEvent(&hum, &temp)) {
    temperature = temp.temperature;
    humidity = hum.relative_humidity;
  }
}

// ================================================================
// UTF-8 -> CP1251 (для русского шрифта на TFT)
// ================================================================
char *utf8rus2(char *source) {
  int i, j, k;
  unsigned char n;
  char m[2] = { '0', '\0' };
  strcpy(target, ""); k = strlen(source); i = j = 0;
  while (i < k) {
    n = source[i]; i++;
    if (n >= 127) {
      switch (n) {
        case 208: {
          n = source[i]; i++;
          if (n == 129) { n = 192; break; }
          break;
        }
        case 209: {
          n = source[i]; i++;
          if (n == 145) { n = 193; break; }
          break;
        }
      }
    }
    m[0] = n; strcat(target, m);
    j++; if (j >= maxString) break;
  }
  return target;
}
