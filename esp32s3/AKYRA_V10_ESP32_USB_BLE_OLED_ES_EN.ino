#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// BLE HID del core ESP32
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEHIDDevice.h>
#include <BLEUtils.h>

// USB HID del ESP32-S3
#include "USB.h"
#include "USBHIDMouse.h"
#include "USBHIDKeyboard.h"
#include "USBHIDConsumerControl.h"

// ============================================================
// AKYRA V10 STABLE VOICE - ESP32 FIRMWARE
// ESP32-S3 + USB CDC/HID + BLE HID + OLED + botón
//
// TARGET USB  = controla el equipo conectado por USB.
// TARGET BLE  = controla el equipo emparejado por Bluetooth.
// TARGET BOTH = envía la misma acción a ambos.
//
// Botón:
// 1 toque  = modo siguiente
// 2 toques = intercambiar roles de manos
// 3 toques = target USB -> BLE -> BOTH
// 4 toques = idioma OLED ESPANOL <-> ENGLISH
// largo    = LOCK / UNLOCK
// ============================================================

#define OLED_SDA 8
#define OLED_SCL 9
#define BUTTON_PIN 13
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define SERIAL_BAUD 115200
#define RX_BUFFER_SIZE 192

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

USBHIDMouse UsbMouse;
USBHIDKeyboard UsbKeyboard;
USBHIDConsumerControl UsbConsumer;

BLEHIDDevice* BleHid = nullptr;
BLECharacteristic* BleInput = nullptr;
BLEServer* BleServerPtr = nullptr;

volatile bool bleConnected = false;
bool oledOK = false;
bool locked = false;
bool draggingUSB = false;
bool draggingBLE = false;

String lastAction = "Arrancando";
String activeProfile = "MOUSE";
String cursorRole = "L";
String actionRole = "R";

unsigned long lastOLEDRefresh = 0;
unsigned long lastPcCommand = 0;
unsigned long lastAnim = 0;
uint8_t animFrame = 0;

char rxBuffer[RX_BUFFER_SIZE];
size_t rxPos = 0;

// ------------------------------------------------------------
// Modos y target
// ------------------------------------------------------------
enum ControlMode : uint8_t {
  MODE_MOUSE = 0,
  MODE_MEDIA = 1,
  MODE_PRESENT = 2,
  MODE_NAV = 3
};

enum OutputTarget : uint8_t {
  TARGET_USB = 0,
  TARGET_BLE = 1,
  TARGET_BOTH = 2
};

ControlMode currentMode = MODE_MOUSE;
OutputTarget outputTarget = TARGET_USB;

// Idioma de la OLED. 4 toques al boton alternan ES <-> EN.
enum OledLanguage : uint8_t { LANG_ES = 0, LANG_EN = 1 };
OledLanguage oledLanguage = LANG_ES;

const char* modeName() {
  switch (currentMode) {
    case MODE_MOUSE: return "MOUSE";
    case MODE_MEDIA: return "MEDIA";
    case MODE_PRESENT: return "PRESENT";
    case MODE_NAV: return "NAV";
  }
  return "?";
}

const char* targetName() {
  switch (outputTarget) {
    case TARGET_USB: return "USB";
    case TARGET_BLE: return "BLE";
    case TARGET_BOTH: return "BOTH";
  }
  return "?";
}

bool wantsUSB() {
  return outputTarget == TARGET_USB || outputTarget == TARGET_BOTH;
}

bool wantsBLE() {
  return outputTarget == TARGET_BLE || outputTarget == TARGET_BOTH;
}

// ------------------------------------------------------------
// BLE HID: un solo Input Report combinado
// Teclado(8B) + Mouse(4B) + Consumer(1B) = 13 bytes.
// Un solo Report Characteristic evita depender de múltiples
// characteristics HID con el mismo UUID.
// ------------------------------------------------------------

struct __attribute__((packed)) BleCombinedReport {
  uint8_t modifiers;
  uint8_t reserved;
  uint8_t keys[6];

  uint8_t mouseButtons;
  int8_t mouseX;
  int8_t mouseY;
  int8_t wheel;

  uint8_t consumer;
};

BleCombinedReport bleReport = {0};

// Report ID 1 para teclado + mouse + consumer.
static const uint8_t BLE_REPORT_MAP[] = {
  // ---------------- Keyboard ----------------
  0x05, 0x01,       // Usage Page (Generic Desktop)
  0x09, 0x06,       // Usage (Keyboard)
  0xA1, 0x01,       // Collection (Application)
  0x85, 0x01,       //   Report ID (1)
  0x05, 0x07,       //   Usage Page (Keyboard)
  0x19, 0xE0,       //   Usage Min (Left Control)
  0x29, 0xE7,       //   Usage Max (Right GUI)
  0x15, 0x00,
  0x25, 0x01,
  0x75, 0x01,
  0x95, 0x08,
  0x81, 0x02,       //   Input modifiers
  0x95, 0x01,
  0x75, 0x08,
  0x81, 0x01,       //   Reserved
  0x95, 0x06,
  0x75, 0x08,
  0x15, 0x00,
  0x25, 0x65,
  0x05, 0x07,
  0x19, 0x00,
  0x29, 0x65,
  0x81, 0x00,       //   6 keys
  0xC0,

  // ---------------- Mouse ----------------
  0x05, 0x01,
  0x09, 0x02,
  0xA1, 0x01,
  0x85, 0x01,       // Same combined report ID
  0x09, 0x01,
  0xA1, 0x00,
  0x05, 0x09,
  0x19, 0x01,
  0x29, 0x05,
  0x15, 0x00,
  0x25, 0x01,
  0x95, 0x05,
  0x75, 0x01,
  0x81, 0x02,       // 5 buttons
  0x95, 0x01,
  0x75, 0x03,
  0x81, 0x01,       // padding
  0x05, 0x01,
  0x09, 0x30,       // X
  0x09, 0x31,       // Y
  0x09, 0x38,       // Wheel
  0x15, 0x81,
  0x25, 0x7F,
  0x75, 0x08,
  0x95, 0x03,
  0x81, 0x06,       // relative
  0xC0,
  0xC0,

  // ---------------- Consumer ----------------
  0x05, 0x0C,       // Consumer
  0x09, 0x01,       // Consumer Control
  0xA1, 0x01,
  0x85, 0x01,
  0x15, 0x00,
  0x25, 0x01,
  0x75, 0x01,
  0x95, 0x06,
  0x09, 0xCD,       // Play/Pause
  0x09, 0xE2,       // Mute
  0x09, 0xE9,       // Volume +
  0x09, 0xEA,       // Volume -
  0x09, 0xB5,       // Next Track
  0x09, 0xB6,       // Previous Track
  0x81, 0x02,
  0x75, 0x01,
  0x95, 0x02,
  0x81, 0x01,
  0xC0
};

class HoloBleServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) override {
    bleConnected = true;
  }

  void onDisconnect(BLEServer* pServer) override {
    bleConnected = false;
    delay(40);
    BLEDevice::startAdvertising();
  }
};

HoloBleServerCallbacks bleCallbacks;

void bleSendReport() {
  if (!bleConnected || BleInput == nullptr) return;
  BleInput->setValue((uint8_t*)&bleReport, sizeof(bleReport));
  BleInput->notify();
}

void bleResetMotion() {
  bleReport.mouseX = 0;
  bleReport.mouseY = 0;
  bleReport.wheel = 0;
}

void bleMove(int dx, int dy, int wheel = 0) {
  if (!bleConnected) return;
  bleReport.mouseX = (int8_t)constrain(dx, -127, 127);
  bleReport.mouseY = (int8_t)constrain(dy, -127, 127);
  bleReport.wheel = (int8_t)constrain(wheel, -127, 127);
  bleSendReport();
  bleResetMotion();
}

void bleMousePress(uint8_t mask) {
  if (!bleConnected) return;
  bleReport.mouseButtons |= mask;
  bleSendReport();
}

void bleMouseRelease(uint8_t mask) {
  if (!bleConnected) return;
  bleReport.mouseButtons &= ~mask;
  bleSendReport();
}

void bleMouseClick(uint8_t mask) {
  bleMousePress(mask);
  delay(18);
  bleMouseRelease(mask);
}

void bleReleaseKeyboard() {
  bleReport.modifiers = 0;
  for (int i = 0; i < 6; i++) bleReport.keys[i] = 0;
  bleSendReport();
}

void bleTapUsage(uint8_t usage, uint8_t modifiers = 0) {
  if (!bleConnected) return;
  bleReport.modifiers = modifiers;
  bleReport.keys[0] = usage;
  for (int i = 1; i < 6; i++) bleReport.keys[i] = 0;
  bleSendReport();
  delay(24);
  bleReleaseKeyboard();
}

void bleConsumerTap(uint8_t mask) {
  if (!bleConnected) return;
  bleReport.consumer = mask;
  bleSendReport();
  delay(24);
  bleReport.consumer = 0;
  bleSendReport();
}

// HID usage IDs
const uint8_t HID_ENTER = 0x28;
const uint8_t HID_ESC = 0x29;
const uint8_t HID_TAB = 0x2B;
const uint8_t HID_MINUS = 0x2D;
const uint8_t HID_EQUAL = 0x2E;
const uint8_t HID_0 = 0x27;
const uint8_t HID_F5 = 0x3E;
const uint8_t HID_PAGE_UP = 0x4B;
const uint8_t HID_PAGE_DOWN = 0x4E;
const uint8_t HID_RIGHT = 0x4F;
const uint8_t HID_LEFT = 0x50;
const uint8_t HID_DOWN = 0x51;
const uint8_t HID_UP = 0x52;

const uint8_t MOD_LCTRL = 0x01;
const uint8_t MOD_LSHIFT = 0x02;
const uint8_t MOD_LALT = 0x04;
const uint8_t MOD_LGUI = 0x08;

// Consumer bitmap
const uint8_t BLE_MEDIA_PLAY_PAUSE = 1 << 0;
const uint8_t BLE_MEDIA_MUTE = 1 << 1;
const uint8_t BLE_MEDIA_VOL_UP = 1 << 2;
const uint8_t BLE_MEDIA_VOL_DOWN = 1 << 3;
const uint8_t BLE_MEDIA_NEXT = 1 << 4;
const uint8_t BLE_MEDIA_PREV = 1 << 5;

uint8_t letterUsage(char c) {
  c = tolower(c);
  if (c >= 'a' && c <= 'z') return 0x04 + (c - 'a');
  return 0;
}

void initBLEHID() {
  BLEDevice::init("AKYRA V10");
  BleServerPtr = BLEDevice::createServer();
  BleServerPtr->setCallbacks(&bleCallbacks);

  BleHid = new BLEHIDDevice(BleServerPtr);
  BleHid->manufacturer()->setValue("AKYRA");
  BleHid->pnp(0x02, 0x303A, 0x4854, 0x0850);
  BleHid->hidInfo(0x00, 0x01);

  BleInput = BleHid->inputReport(1);
  BleHid->reportMap((uint8_t*)BLE_REPORT_MAP, sizeof(BLE_REPORT_MAP));
  BleHid->startServices();
  BleHid->setBatteryLevel(100);

  BLEAdvertising* advertising = BLEDevice::getAdvertising();
  advertising->setAppearance(HID_KEYBOARD);
  advertising->addServiceUUID(BleHid->hidService()->getUUID());
  advertising->start();
}

// ------------------------------------------------------------
// Estado / OLED
// ------------------------------------------------------------

void setAction(const String &s) {
  lastAction = s;
}

String oledTrim(String s, uint8_t maxChars) {
  if (s.length() <= maxChars) return s;
  return s.substring(0, maxChars);
}

const char* langName() {
  return oledLanguage == LANG_ES ? "ES" : "EN";
}

const char* oledModeName() {
  if (oledLanguage == LANG_ES) {
    switch (currentMode) {
      case MODE_MOUSE: return "RATON";
      case MODE_MEDIA: return "MEDIA";
      case MODE_PRESENT: return "PRES";
      case MODE_NAV: return "NAV";
    }
  }
  return modeName();
}

const char* oledLiveName() {
  if (locked) return oledLanguage == LANG_ES ? "BLOQ" : "LOCK";
  return oledLanguage == LANG_ES ? "ACT" : "LIVE";
}

const char* oledTargetLabel() {
  return oledLanguage == LANG_ES ? "SAL" : "TGT";
}

const char* oledAppLabel() {
  return oledLanguage == LANG_ES ? "APP" : "APP";
}

void drawOLED(bool force = false);

void toggleOLEDLanguage() {
  oledLanguage = (oledLanguage == LANG_ES) ? LANG_EN : LANG_ES;
  setAction(oledLanguage == LANG_ES ? "Idioma ES" : "Lang EN");
  drawOLED(true);
}

void initOLED() {
  Wire.begin(OLED_SDA, OLED_SCL);
  delay(60);

  if (display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) oledOK = true;
  else if (display.begin(SSD1306_SWITCHCAPVCC, 0x3D)) oledOK = true;

  if (oledOK) {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(28, 8);
    display.println("AKYRA V10");
    display.setCursor(14, 24);
    display.println("USB + BLE HID");
    display.setCursor(11, 40);
    display.println("ESPANOL / ENGLISH");
    display.setCursor(31, 54);
    display.println("4x = ES/EN");
    display.display();
    delay(700);
  }
}

void drawOLED(bool force) {
  if (!oledOK) return;
  if (!force && millis() - lastOLEDRefresh < 120) return;
  lastOLEDRefresh = millis();

  if (millis() - lastAnim > 220) {
    lastAnim = millis();
    animFrame = (animFrame + 1) % 4;
  }

  bool pcOnline = (millis() - lastPcCommand) < 2600UL;
  const char spinner[4] = {'|', '/', '-', '\\'};

  display.clearDisplay();
  display.fillRect(0, 0, 128, 12, SSD1306_WHITE);
  display.setTextColor(SSD1306_BLACK);
  display.setCursor(3, 2);
  display.print("AKYRA V10 ");
  display.print(langName());
  display.setCursor(112, 2);
  display.print(pcOnline ? spinner[animFrame] : 'x');

  display.setTextColor(SSD1306_WHITE);
  display.setCursor(2, 16);
  display.print(oledModeName());
  display.setCursor(78, 16);
  display.print(oledLiveName());

  display.setCursor(2, 27);
  display.print(oledTargetLabel());
  display.print(" ");
  display.print(targetName());
  display.setCursor(78, 27);
  display.print("BT:");
  display.print(bleConnected ? "OK" : "--");

  display.setCursor(2, 38);
  display.print(cursorRole);
  display.print(":CUR ");
  display.print(actionRole);
  display.print(":ACT");

  display.setCursor(2, 49);
  display.print(oledAppLabel());
  display.print(" ");
  display.print(oledTrim(activeProfile, 14));

  display.drawFastHLine(0, 58, 128, SSD1306_WHITE);
  // última acción muy corta en la última línea
  display.setCursor(76, 49);
  display.print(oledTrim(lastAction, 8));

  display.display();
}

// ------------------------------------------------------------
// USB/BLE Output Manager
// ------------------------------------------------------------

void releaseEverything() {
  if (draggingUSB) {
    UsbMouse.release(MOUSE_LEFT);
    draggingUSB = false;
  }
  UsbMouse.release(MOUSE_LEFT);
  UsbMouse.release(MOUSE_RIGHT);
  UsbMouse.release(MOUSE_MIDDLE);
  UsbKeyboard.releaseAll();
  UsbConsumer.release();

  if (draggingBLE) {
    bleMouseRelease(1);
    draggingBLE = false;
  }
  bleReport.mouseButtons = 0;
  bleReport.modifiers = 0;
  bleReport.consumer = 0;
  for (int i = 0; i < 6; i++) bleReport.keys[i] = 0;
  bleResetMotion();
  bleSendReport();
}

void setMode(uint8_t m) {
  if (m > MODE_NAV) return;
  releaseEverything();
  currentMode = (ControlMode)m;
  setAction(String("Modo ") + modeName());
}

void nextMode() {
  setMode(((uint8_t)currentMode + 1) % 4);
}

void setTarget(OutputTarget t) {
  releaseEverything();
  outputTarget = t;
  setAction(String("TGT ") + targetName());
}

void nextTarget() {
  setTarget((OutputTarget)(((uint8_t)outputTarget + 1) % 3));
}

void toggleLock() {
  locked = !locked;
  releaseEverything();
  setAction(locked ? (oledLanguage == LANG_ES ? "BLOQUEO" : "LOCKED") : (oledLanguage == LANG_ES ? "ACTIVO" : "ACTIVE"));
}

void usbTapKey(uint8_t key, uint16_t holdMs = 28) {
  UsbKeyboard.press(key);
  delay(holdMs);
  UsbKeyboard.release(key);
}

void usbHotkey(uint8_t mod, uint8_t key) {
  UsbKeyboard.press(mod);
  delay(12);
  UsbKeyboard.press(key);
  delay(34);
  UsbKeyboard.release(key);
  UsbKeyboard.release(mod);
}

void usbConsumerTap(uint16_t code) {
  UsbConsumer.press(code);
  delay(22);
  UsbConsumer.release();
}

void outMove(int dx, int dy) {
  if (wantsUSB()) UsbMouse.move(dx, dy);
  if (wantsBLE()) bleMove(dx, dy, 0);
}

void outScroll(int wheel) {
  if (wantsUSB()) UsbMouse.move(0, 0, wheel);
  if (wantsBLE()) bleMove(0, 0, wheel);
}

void outLeftClick() {
  if (wantsUSB()) UsbMouse.click(MOUSE_LEFT);
  if (wantsBLE()) bleMouseClick(1);
}

void outRightClick() {
  if (wantsUSB()) UsbMouse.click(MOUSE_RIGHT);
  if (wantsBLE()) bleMouseClick(2);
}

void outDragStart() {
  if (wantsUSB() && !draggingUSB) {
    UsbMouse.press(MOUSE_LEFT);
    draggingUSB = true;
  }
  if (wantsBLE() && !draggingBLE && bleConnected) {
    bleMousePress(1);
    draggingBLE = true;
  }
}

void outDragEnd() {
  if (draggingUSB) UsbMouse.release(MOUSE_LEFT);
  if (draggingBLE) bleMouseRelease(1);
  draggingUSB = false;
  draggingBLE = false;
}

void bothKeyTap(uint8_t usbKey, uint8_t bleUsage) {
  if (wantsUSB()) usbTapKey(usbKey);
  if (wantsBLE()) bleTapUsage(bleUsage);
}

void bothHotkey(uint8_t usbMod, uint8_t usbKey, uint8_t bleMod, uint8_t bleUsage) {
  if (wantsUSB()) usbHotkey(usbMod, usbKey);
  if (wantsBLE()) bleTapUsage(bleUsage, bleMod);
}

void bothHotkeyChar(uint8_t usbMod, char ch, uint8_t bleMod) {
  if (wantsUSB()) {
    UsbKeyboard.press(usbMod);
    delay(12);
    UsbKeyboard.press(ch);
    delay(34);
    UsbKeyboard.release(ch);
    UsbKeyboard.release(usbMod);
  }
  if (wantsBLE()) {
    uint8_t usage = letterUsage(ch);
    if (usage) bleTapUsage(usage, bleMod);
  }
}

void runAction(String a) {
  a.trim();
  a.toUpperCase();

  if (a == "CANCEL") {
    releaseEverything();
    setAction("CANCEL");
    return;
  }

  if (locked) return;

  if (a == "CLICK") {
    outLeftClick(); setAction("Click");
  }
  else if (a == "RIGHTCLICK") {
    outRightClick(); setAction("RClick");
  }
  else if (a == "DOUBLECLICK") {
    outLeftClick(); delay(80); outLeftClick(); setAction("2Click");
  }
  else if (a == "ALT_TAB") {
    bothHotkey(KEY_LEFT_ALT, KEY_TAB, MOD_LALT, HID_TAB); setAction("AltTab");
  }
  else if (a == "WIN_TAB") {
    bothHotkey(KEY_LEFT_GUI, KEY_TAB, MOD_LGUI, HID_TAB); setAction("WinTab");
  }
  else if (a == "BACK") {
    bothHotkey(KEY_LEFT_ALT, KEY_LEFT_ARROW, MOD_LALT, HID_LEFT); setAction("Atras");
  }
  else if (a == "FORWARD") {
    bothHotkey(KEY_LEFT_ALT, KEY_RIGHT_ARROW, MOD_LALT, HID_RIGHT); setAction("Adelante");
  }
  else if (a == "PAGE_UP") {
    bothKeyTap(KEY_PAGE_UP, HID_PAGE_UP); setAction("PgUp");
  }
  else if (a == "PAGE_DOWN") {
    bothKeyTap(KEY_PAGE_DOWN, HID_PAGE_DOWN); setAction("PgDn");
  }
  else if (a == "LEFT") {
    bothKeyTap(KEY_LEFT_ARROW, HID_LEFT); setAction("Left");
  }
  else if (a == "RIGHT") {
    bothKeyTap(KEY_RIGHT_ARROW, HID_RIGHT); setAction("Right");
  }
  else if (a == "UP") {
    bothKeyTap(KEY_UP_ARROW, HID_UP); setAction("Up");
  }
  else if (a == "DOWN") {
    bothKeyTap(KEY_DOWN_ARROW, HID_DOWN); setAction("Down");
  }
  else if (a == "ENTER") {
    bothKeyTap(KEY_RETURN, HID_ENTER); setAction("Enter");
  }
  else if (a == "ESC") {
    bothKeyTap(KEY_ESC, HID_ESC); setAction("Esc");
  }
  else if (a == "F5") {
    bothKeyTap(KEY_F5, HID_F5); setAction("F5");
  }
  else if (a == "FULLSCREEN") {
    if (wantsUSB()) usbTapKey('f');
    if (wantsBLE()) bleTapUsage(letterUsage('f'));
    setAction("Full");
  }
  else if (a == "WIN_D") {
    bothHotkeyChar(KEY_LEFT_GUI, 'd', MOD_LGUI); setAction("Desk");
  }
  else if (a == "COPY") {
    bothHotkeyChar(KEY_LEFT_CTRL, 'c', MOD_LCTRL); setAction("Copy");
  }
  else if (a == "PASTE") {
    bothHotkeyChar(KEY_LEFT_CTRL, 'v', MOD_LCTRL); setAction("Paste");
  }
  else if (a == "CUT") {
    bothHotkeyChar(KEY_LEFT_CTRL, 'x', MOD_LCTRL); setAction("Cut");
  }
  else if (a == "UNDO") {
    bothHotkeyChar(KEY_LEFT_CTRL, 'z', MOD_LCTRL); setAction("Undo");
  }
  else if (a == "REDO") {
    bothHotkeyChar(KEY_LEFT_CTRL, 'y', MOD_LCTRL); setAction("Redo");
  }
  else if (a == "NEW_TAB") {
    bothHotkeyChar(KEY_LEFT_CTRL, 't', MOD_LCTRL); setAction("NewTab");
  }
  else if (a == "CLOSE_TAB") {
    bothHotkeyChar(KEY_LEFT_CTRL, 'w', MOD_LCTRL); setAction("ClsTab");
  }
  else if (a == "ZOOM_IN") {
    if (wantsUSB()) usbHotkey(KEY_LEFT_CTRL, '=');
    if (wantsBLE()) bleTapUsage(HID_EQUAL, MOD_LCTRL);
    setAction("Zoom+");
  }
  else if (a == "ZOOM_OUT") {
    if (wantsUSB()) usbHotkey(KEY_LEFT_CTRL, '-');
    if (wantsBLE()) bleTapUsage(HID_MINUS, MOD_LCTRL);
    setAction("Zoom-");
  }
  else if (a == "ZOOM_RESET") {
    if (wantsUSB()) usbHotkey(KEY_LEFT_CTRL, '0');
    if (wantsBLE()) bleTapUsage(HID_0, MOD_LCTRL);
    setAction("Zoom100");
  }
  else if (a == "PLAY_PAUSE") {
    if (wantsUSB()) usbConsumerTap(CONSUMER_CONTROL_PLAY_PAUSE);
    if (wantsBLE()) bleConsumerTap(BLE_MEDIA_PLAY_PAUSE);
    setAction("Play");
  }
  else if (a == "MUTE") {
    if (wantsUSB()) usbConsumerTap(CONSUMER_CONTROL_MUTE);
    if (wantsBLE()) bleConsumerTap(BLE_MEDIA_MUTE);
    setAction("Mute");
  }
  else if (a == "VOL_UP") {
    if (wantsUSB()) usbConsumerTap(CONSUMER_CONTROL_VOLUME_INCREMENT);
    if (wantsBLE()) bleConsumerTap(BLE_MEDIA_VOL_UP);
    setAction("Vol+");
  }
  else if (a == "VOL_DOWN") {
    if (wantsUSB()) usbConsumerTap(CONSUMER_CONTROL_VOLUME_DECREMENT);
    if (wantsBLE()) bleConsumerTap(BLE_MEDIA_VOL_DOWN);
    setAction("Vol-");
  }
  else if (a == "NEXT_TRACK") {
    if (wantsUSB()) usbConsumerTap(CONSUMER_CONTROL_SCAN_NEXT);
    if (wantsBLE()) bleConsumerTap(BLE_MEDIA_NEXT);
    setAction("Next");
  }
  else if (a == "PREV_TRACK") {
    if (wantsUSB()) usbConsumerTap(CONSUMER_CONTROL_SCAN_PREVIOUS);
    if (wantsBLE()) bleConsumerTap(BLE_MEDIA_PREV);
    setAction("Prev");
  }
}

// ------------------------------------------------------------
// Protocolo Serial V8.5
// ------------------------------------------------------------

void sendStatus(const char* prefix = "STATUS") {
  Serial.print(prefix); Serial.print(',');
  Serial.print(modeName()); Serial.print(',');
  Serial.print(locked ? '1' : '0'); Serial.print(',');
  Serial.print(lastAction); Serial.print(',');
  Serial.print(cursorRole); Serial.print(',');
  Serial.print(actionRole); Serial.print(',');
  Serial.print(targetName()); Serial.print(',');
  Serial.println(bleConnected ? '1' : '0');
}

void sendHello() {
  Serial.print("HOLOTOUCH_USB,V8.5,");
  Serial.print(modeName()); Serial.print(',');
  Serial.print(locked ? '1' : '0'); Serial.print(',');
  Serial.print(cursorRole); Serial.print(',');
  Serial.print(actionRole); Serial.print(',');
  Serial.print(targetName()); Serial.print(',');
  Serial.println(bleConnected ? '1' : '0');
}

void processCommand(String cmd) {
  cmd.trim();
  if (!cmd.length()) return;
  lastPcCommand = millis();

  if (cmd == "HELLO") { sendHello(); return; }
  if (cmd == "PING") { Serial.println("PONG"); return; }
  if (cmd == "STATUS") { sendStatus(); return; }

  if (cmd == "MODE_NEXT") {
    nextMode(); sendStatus(); return;
  }

  if (cmd == "LOCK_TOGGLE") {
    toggleLock(); sendStatus(); return;
  }

  if (cmd == "TARGET_NEXT") {
    nextTarget(); sendStatus(); return;
  }

  if (cmd.startsWith("TARGET,")) {
    String t = cmd.substring(7);
    t.trim(); t.toUpperCase();
    if (t == "USB") setTarget(TARGET_USB);
    else if (t == "BLE") setTarget(TARGET_BLE);
    else if (t == "BOTH") setTarget(TARGET_BOTH);
    sendStatus();
    return;
  }

  if (cmd.startsWith("MODE,")) {
    String m = cmd.substring(5);
    m.toUpperCase();
    if (m == "MOUSE") setMode(MODE_MOUSE);
    else if (m == "MEDIA") setMode(MODE_MEDIA);
    else if (m == "PRESENT") setMode(MODE_PRESENT);
    else if (m == "NAV") setMode(MODE_NAV);
    sendStatus();
    return;
  }

  if (cmd == "LANG_TOGGLE") {
    toggleOLEDLanguage();
    Serial.print("EVENT,LANG,"); Serial.println(langName());
    return;
  }

  if (cmd.startsWith("LANG,")) {
    String l = cmd.substring(5);
    l.trim(); l.toUpperCase();
    if (l == "ES" || l == "ESP" || l == "SPANISH") oledLanguage = LANG_ES;
    else if (l == "EN" || l == "ENG" || l == "ENGLISH") oledLanguage = LANG_EN;
    setAction(oledLanguage == LANG_ES ? "Idioma ES" : "Lang EN");
    drawOLED(true);
    Serial.print("EVENT,LANG,"); Serial.println(langName());
    return;
  }

  if (cmd.startsWith("PROFILE,")) {
    activeProfile = cmd.substring(8);
    activeProfile.trim();
    activeProfile.toUpperCase();
    if (activeProfile.length() > 14) activeProfile = activeProfile.substring(0, 14);
    return;
  }

  if (cmd.startsWith("ROLE,")) {
    int p = cmd.indexOf(',', 5);
    if (p > 0) {
      cursorRole = cmd.substring(5, p);
      actionRole = cmd.substring(p + 1);
      cursorRole.trim();
      actionRole.trim();
    }
    return;
  }

  if (locked) return;

  if (cmd.startsWith("MOVE,")) {
    int c1 = cmd.indexOf(',');
    int c2 = cmd.indexOf(',', c1 + 1);
    if (c2 > c1) {
      int dx = constrain(cmd.substring(c1 + 1, c2).toInt(), -127, 127);
      int dy = constrain(cmd.substring(c2 + 1).toInt(), -127, 127);
      outMove(dx, dy);
    }
    return;
  }

  if (cmd.startsWith("SCROLL,")) {
    int wheel = constrain(cmd.substring(7).toInt(), -12, 12);
    outScroll(wheel);
    setAction("Scroll");
    return;
  }

  if (cmd == "DRAG_START") {
    outDragStart();
    setAction("Drag");
    return;
  }

  if (cmd == "DRAG_END") {
    outDragEnd();
    setAction("Drop");
    return;
  }

  if (cmd.startsWith("ACT,")) {
    runAction(cmd.substring(4));
    return;
  }

  // compat V6/V7
  if (cmd == "PRIMARY" || cmd == "CLICK") runAction("CLICK");
  else if (cmd == "SECONDARY" || cmd == "RIGHTCLICK") runAction("RIGHTCLICK");
  else if (cmd == "THREE" || cmd == "ALT_TAB") runAction("ALT_TAB");
  else if (cmd == "SWIPE_LEFT") runAction("BACK");
  else if (cmd == "SWIPE_RIGHT") runAction("FORWARD");
  else if (cmd == "SWIPE_UP") runAction("PAGE_UP");
  else if (cmd == "SWIPE_DOWN") runAction("PAGE_DOWN");
}

void readUSBCommands() {
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\r') continue;

    if (c == '\n') {
      rxBuffer[rxPos] = '\0';
      if (rxPos > 0) processCommand(String(rxBuffer));
      rxPos = 0;
      continue;
    }

    if (rxPos < RX_BUFFER_SIZE - 1) rxBuffer[rxPos++] = c;
    else rxPos = 0;
  }
}

// ------------------------------------------------------------
// Botón: 1/2/3 clics + largo
// ------------------------------------------------------------

bool lastRawButton = HIGH;
bool stableButton = HIGH;
unsigned long lastDebounceTime = 0;
unsigned long buttonPressStart = 0;
bool longPressDone = false;
uint8_t clickCount = 0;
unsigned long lastClickAt = 0;

const unsigned long DEBOUNCE_MS = 35;
const unsigned long LONG_PRESS_MS = 900;
const unsigned long MULTI_CLICK_MS = 420;

void resolveClicks() {
  if (clickCount == 0) return;

  if (clickCount == 1) {
    nextMode();
    Serial.println("EVENT,MODE_NEXT");
  } else if (clickCount == 2) {
    Serial.println("EVENT,SWAP_ROLES");
    setAction("Swap");
  } else if (clickCount == 3) {
    nextTarget();
    Serial.println("EVENT,TARGET_NEXT");
  } else {
    toggleOLEDLanguage();
    Serial.print("EVENT,LANG,");
    Serial.println(langName());
  }

  clickCount = 0;
  sendStatus();
}

void handleButton() {
  bool reading = digitalRead(BUTTON_PIN);

  if (reading != lastRawButton) lastDebounceTime = millis();

  if (millis() - lastDebounceTime > DEBOUNCE_MS && reading != stableButton) {
    stableButton = reading;

    if (stableButton == LOW) {
      buttonPressStart = millis();
      longPressDone = false;
    } else if (!longPressDone) {
      clickCount++;
      lastClickAt = millis();
    }
  }

  if (stableButton == LOW && !longPressDone &&
      millis() - buttonPressStart >= LONG_PRESS_MS) {
    longPressDone = true;
    clickCount = 0;
    toggleLock();
    sendStatus();
  }

  if (clickCount > 0 && stableButton == HIGH &&
      millis() - lastClickAt > MULTI_CLICK_MS) {
    resolveClicks();
  }

  lastRawButton = reading;
}

// ------------------------------------------------------------
// Setup / Loop
// ------------------------------------------------------------

void setup() {
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  initOLED();

  // USB HID
  UsbMouse.begin();
  UsbKeyboard.begin();
  UsbConsumer.begin();
  USB.begin();

  Serial.begin(SERIAL_BAUD);
  delay(500);

  // BLE HID
  initBLEHID();

  setAction(oledLanguage == LANG_ES ? "Listo" : "Ready");
  drawOLED(true);
  sendHello();
}

void loop() {
  readUSBCommands();
  handleButton();
  drawOLED();
  delay(1);
}
