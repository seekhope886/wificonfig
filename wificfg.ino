#include <WiFi.h>
#include <Preferences.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// ==================== 硬體配置 ====================
#define TOUCH_PIN          2
#define SSD1306_SDA        8
#define SSD1306_SCL        9
#define TOUCH_DEBOUNCE_MS  50
#define DOUBLE_CLICK_MS    300
#define LONG_PRESS_MS      2000
#define LONG_LONG_PRESS_MS 5000

// ==================== NVS 配置 ====================
Preferences preferences;
#define NVS_NAMESPACE "wifi_config"
#define KEY_SSID      "ssid"
#define KEY_PASS      "pass"
#define KEY_CONFIGURED "configured"

// ==================== WiFi 配置 ====================
#define MAX_SCAN_RESULTS   8
#define WIFI_TIMEOUT_MS    10000

// ==================== 任務配置 ====================
#define MAIN_TASK_STACK    4096
#define WIFI_TASK_STACK    4096
#define DISPLAY_TASK_STACK 4096
#define MAIN_TASK_PRIORITY 3
#define WIFI_TASK_PRIORITY 2
#define DISPLAY_TASK_PRIORITY 1

// ==================== 系統狀態 ====================
typedef enum {
    STATE_INIT,
    STATE_SCANNING,
    STATE_SELECT_SSID,
    STATE_INPUT_PASS,
    STATE_CONNECTING,
    STATE_CONNECTED,
    STATE_FAILED,
    STATE_CONFIRM_RESET,
    STATE_SHOW_IP
} system_state_t;

system_state_t currentState = STATE_INIT;
system_state_t previousState = STATE_INIT;

// ==================== 觸摸事件 ====================
typedef enum {
    EVENT_NONE,
    EVENT_SINGLE_CLICK,
    EVENT_DOUBLE_CLICK,
    EVENT_TRIPLE_CLICK,
    EVENT_LONG_PRESS,
    EVENT_LONG_LONG_PRESS
} touch_event_t;

unsigned long lastTouchTime = 0;
unsigned long touchStartTime = 0;
bool touchState = false;
bool lastTouchState = false;
int clickCount = 0;
touch_event_t lastEvent = EVENT_NONE;

// ==================== WiFi 變數 ====================
String savedSSID = "";
String savedPass = "";
bool isConfigured = false;
bool autoConnectAttempt = false;
String scanResults[MAX_SCAN_RESULTS];
int scanRSSI[MAX_SCAN_RESULTS];
int scanCount = 0;
int selectedIndex = 0;
bool scanComplete = false;
bool connectionSuccess = false;
String selectedSSID = "";
unsigned long wifiStartTime = 0;

// ==================== 密碼輸入配置 ====================
const char ALL_CHARS[] = "1234567890ABCDEFGHIJKLMNOPQRSTUVWXYZ.-_@";
const int TOTAL_CHARS = sizeof(ALL_CHARS) - 1;
const int OK_INDEX = TOTAL_CHARS;
int passCursor = 0;
char currentInput[64] = "";
int inputLength = 0;
char tempChar = '1';

// ==================== 顯示器物件 ====================
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

// ==================== 任務句柄 ====================
TaskHandle_t mainTaskHandle = NULL;
TaskHandle_t displayTaskHandle = NULL;
SemaphoreHandle_t stateMutex = NULL;

// ==================== 函數宣告 ====================
void mainTask(void* param);
void displayTask(void* param);
touch_event_t getTouchEvent();
void updateTouch();
void drawInitScreen();
void drawScanningScreen();
void drawSelectScreen();
void drawPasswordInput();
void drawConnectingScreen();
void drawConnectedScreen();
void drawFailedScreen();
void drawConfirmReset();
void drawShowIP();
void scanWiFiNetworks();
bool connectToWiFi(const char* ssid, const char* pass);
bool loadCredentials();
void saveCredentials(const char* ssid, const char* pass);
void resetAllSettings();
bool matchSavedNetwork();

// ==================== 設定 ====================
void setup() {
    delay(500);
    Serial.begin(115200);
    Serial.println("=== ESP32 WiFi Config ===");
    
    Wire.begin(SSD1306_SDA, SSD1306_SCL);
    pinMode(TOUCH_PIN, INPUT);
    
    u8g2.begin();
    u8g2.setFont(u8g2_font_5x8_tr);
    u8g2.clearBuffer();
    drawInitScreen();
    u8g2.sendBuffer();
    
    stateMutex = xSemaphoreCreateMutex();
    
    xTaskCreatePinnedToCore(mainTask, "MainTask", MAIN_TASK_STACK, NULL, MAIN_TASK_PRIORITY, &mainTaskHandle, 0);
    xTaskCreatePinnedToCore(displayTask, "DisplayTask", DISPLAY_TASK_STACK, NULL, DISPLAY_TASK_PRIORITY, &displayTaskHandle, 1);
}

void loop() {
    vTaskDelay(pdMS_TO_TICKS(100));
}

// ==================== 主任務 ====================
void mainTask(void* param) {
    vTaskDelay(pdMS_TO_TICKS(500));
    
    if (!loadCredentials()) {
        Serial.println("No saved credentials");
    }
    
    currentState = STATE_SCANNING;
    Serial.println("Starting WiFi scan...");
    
    while (true) {
        touch_event_t event = getTouchEvent();
        
        switch (currentState) {
            case STATE_INIT:
                vTaskDelay(pdMS_TO_TICKS(100));
                break;
                
            case STATE_SCANNING:
                scanWiFiNetworks();
                if (scanCount > 0) {
                    if (matchSavedNetwork()) {
                        Serial.println("Found saved network, connecting...");
                        autoConnectAttempt = true;
                        currentState = STATE_CONNECTING;
                        wifiStartTime = millis();
                    } else {
                        autoConnectAttempt = false;
                        currentState = STATE_SELECT_SSID;
                        selectedIndex = 0;
                    }
                }
                break;
                
            case STATE_SELECT_SSID:
                if (event == EVENT_SINGLE_CLICK) {
                    selectedIndex = (selectedIndex + 1) % (scanCount + 1);
                } else if (event == EVENT_DOUBLE_CLICK || event == EVENT_SINGLE_CLICK) {
                    if (event == EVENT_DOUBLE_CLICK || selectedIndex < scanCount) {
                        if (selectedIndex < scanCount) {
                            selectedSSID = scanResults[selectedIndex];
                            memset(currentInput, 0, sizeof(currentInput));
                            inputLength = 0;
                            passCursor = 0;
                            tempChar = '1';
                            currentState = STATE_INPUT_PASS;
                        }
                    }
                } else if (event == EVENT_LONG_LONG_PRESS) {
                    resetAllSettings();
                    currentState = STATE_SCANNING;
                }
                break;
                
            case STATE_INPUT_PASS:
                handlePasswordInput(event);
                break;
                
            case STATE_CONNECTING:
                if (connectToWiFi(selectedSSID.c_str(), currentInput)) {
                    connectionSuccess = true;
                    autoConnectAttempt = false;
                    currentState = STATE_CONNECTED;
                } else {
                    connectionSuccess = false;
                    if (autoConnectAttempt) {
                        currentState = STATE_CONFIRM_RESET;
                    } else {
                        currentState = STATE_FAILED;
                    }
                }
                break;
                
            case STATE_CONNECTED:
                if (event == EVENT_SINGLE_CLICK) {
                    currentState = STATE_SHOW_IP;
                } else if (event == EVENT_LONG_LONG_PRESS) {
                    resetAllSettings();
                    currentState = STATE_SCANNING;
                } else if (WiFi.status() != WL_CONNECTED) {
                    Serial.println("WiFi disconnected, reconnecting...");
                    currentState = STATE_CONNECTING;
                    wifiStartTime = millis();
                }
                break;
                
            case STATE_FAILED:
                if (event == EVENT_SINGLE_CLICK || event == EVENT_DOUBLE_CLICK) {
                    currentState = STATE_SELECT_SSID;
                    selectedIndex = 0;
                } else if (event == EVENT_LONG_LONG_PRESS) {
                    resetAllSettings();
                    currentState = STATE_SCANNING;
                }
                break;
                
            case STATE_CONFIRM_RESET:
                if (event == EVENT_SINGLE_CLICK) {
                    resetAllSettings();
                    currentState = STATE_SCANNING;
                } else if (event == EVENT_DOUBLE_CLICK) {
                    autoConnectAttempt = false;
                    currentState = STATE_SELECT_SSID;
                    selectedIndex = 0;
                }
                break;
                
            case STATE_SHOW_IP:
                if (event == EVENT_SINGLE_CLICK || event == EVENT_DOUBLE_CLICK || event == EVENT_LONG_LONG_PRESS) {
                    currentState = STATE_CONNECTED;
                }
                break;
        }
        
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// ==================== 顯示任務 ====================
void displayTask(void* param) {
    while (true) {
        xSemaphoreTake(stateMutex, portMAX_DELAY);
        system_state_t state = currentState;
        xSemaphoreGive(stateMutex);
        
        switch (state) {
            case STATE_INIT:
                drawInitScreen();
                break;
            case STATE_SCANNING:
                drawScanningScreen();
                break;
            case STATE_SELECT_SSID:
                drawSelectScreen();
                break;
            case STATE_INPUT_PASS:
                drawPasswordInput();
                break;
            case STATE_CONNECTING:
                drawConnectingScreen();
                break;
            case STATE_CONNECTED:
                drawConnectedScreen();
                break;
            case STATE_FAILED:
                drawFailedScreen();
                break;
            case STATE_CONFIRM_RESET:
                drawConfirmReset();
                break;
            case STATE_SHOW_IP:
                drawShowIP();
                break;
        }
        
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// ==================== 觸摸處理 ====================
touch_event_t getTouchEvent() {
    updateTouch();
    touch_event_t evt = lastEvent;
    lastEvent = EVENT_NONE;
    return evt;
}

void updateTouch() {
    lastEvent = EVENT_NONE;
    bool currentState = digitalRead(TOUCH_PIN);
    unsigned long now = millis();
    
    if (currentState && !lastTouchState) {
        touchStartTime = now;
        lastTouchState = true;
        clickCount++;
    }
    
    if (!currentState && lastTouchState) {
        lastTouchState = false;
        lastTouchTime = now;
    }
    
    if (!currentState && clickCount > 0) {
        if ((now - lastTouchTime) > DOUBLE_CLICK_MS) {
            if (clickCount == 1) {
                lastEvent = EVENT_SINGLE_CLICK;
            } else if (clickCount == 2) {
                lastEvent = EVENT_DOUBLE_CLICK;
            } else if (clickCount >= 3) {
                lastEvent = EVENT_TRIPLE_CLICK;
            }
            clickCount = 0;
        }
    }
    
    if (lastTouchState && (now - touchStartTime) > LONG_LONG_PRESS_MS) {
        lastEvent = EVENT_LONG_LONG_PRESS;
        lastTouchState = false;
        clickCount = 0;
    } else if (lastTouchState && (now - touchStartTime) > LONG_PRESS_MS) {
        lastEvent = EVENT_LONG_PRESS;
    }
}

void handlePasswordInput(touch_event_t event) {
    if (event == EVENT_SINGLE_CLICK) {
        for (int i = 0; i < TOTAL_CHARS; i++) {
            if (ALL_CHARS[i] == tempChar) {
                tempChar = ALL_CHARS[(i + 1) % TOTAL_CHARS];
                break;
            }
        }
    } else if (event == EVENT_DOUBLE_CLICK) {
        if (passCursor == OK_INDEX) {
            saveCredentials(selectedSSID.c_str(), currentInput);
            currentState = STATE_CONNECTING;
            wifiStartTime = millis();
        } else {
            if (inputLength < 63) {
                currentInput[inputLength] = tempChar;
                inputLength++;
            }
            passCursor++;
            if (passCursor > TOTAL_CHARS) {
                passCursor = OK_INDEX;
            }
            if (passCursor == OK_INDEX) {
                tempChar = 'O';
            } else {
                tempChar = '1';
            }
        }
    } else if (event == EVENT_TRIPLE_CLICK) {
        if (inputLength > 0) {
            char deletedChar = currentInput[inputLength - 1];
            inputLength--;
            currentInput[inputLength] = '\0';
            if (passCursor > 0) {
                passCursor--;
            }
            if (passCursor == OK_INDEX) {
                tempChar = 'O';
            } else {
                tempChar = deletedChar;
            }
        }
    } else if (event == EVENT_LONG_LONG_PRESS) {
        if (inputLength > 0) {
            saveCredentials(selectedSSID.c_str(), currentInput);
        }
        currentState = STATE_SELECT_SSID;
        selectedIndex = 0;
        passCursor = 0;
        tempChar = '1';
    }
}

// ==================== WiFi 功能 ====================
void scanWiFiNetworks() {
    WiFi.mode(WIFI_STA);
    WiFi.setTxPower(WIFI_POWER_8_5dBm);     
    WiFi.disconnect();
    vTaskDelay(pdMS_TO_TICKS(100));
    
    int16_t n = WiFi.scanNetworks(false, true, false);
    
    if (n > 0) {
        scanCount = min((int16_t)MAX_SCAN_RESULTS, n);
        
        for (int i = 0; i < scanCount; i++) {
            scanResults[i] = WiFi.SSID(i);
            scanRSSI[i] = WiFi.RSSI(i);
        }
        
        for (int i = 0; i < scanCount - 1; i++) {
            for (int j = i + 1; j < scanCount; j++) {
                if (scanRSSI[i] < scanRSSI[j]) {
                    String tempSSID = scanResults[i];
                    scanResults[i] = scanResults[j];
                    scanResults[j] = tempSSID;
                    int tempRSSI = scanRSSI[i];
                    scanRSSI[i] = scanRSSI[j];
                    scanRSSI[j] = tempRSSI;
                }
            }
        }
        
        Serial.printf("Found %d networks\n", scanCount);
        for (int i = 0; i < scanCount; i++) {
            Serial.printf("  %d: %s (%d dBm)\n", i + 1, scanResults[i].c_str(), scanRSSI[i]);
        }
    } else {
        scanCount = 0;
        Serial.println("No networks found");
    }
    
    scanComplete = true;
    WiFi.scanDelete();
}

bool matchSavedNetwork() {
    if (!isConfigured || savedSSID.length() == 0) return false;
    
    for (int i = 0; i < scanCount; i++) {
        if (scanResults[i] == savedSSID) {
            selectedSSID = savedSSID;
            savedPass.toCharArray(currentInput, sizeof(currentInput));
            inputLength = savedPass.length();
            passCursor = inputLength;
            if (passCursor <= TOTAL_CHARS) {
                if (passCursor == OK_INDEX) {
                    tempChar = 'O';
                } else {
                    tempChar = '1';
                }
            } else {
                passCursor = OK_INDEX;
                tempChar = 'O';
            }
            return true;
        }
    }
    return false;
}

bool connectToWiFi(const char* ssid, const char* pass) {
    Serial.printf("Connecting to: %s\n", ssid);
    
    WiFi.disconnect();
    WiFi.begin(ssid, pass);
    WiFi.setTxPower(WIFI_POWER_8_5dBm);     
    
    unsigned long startTime = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - startTime) < WIFI_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("Connected! IP: %s\n", WiFi.localIP().toString().c_str());
        return true;
    } else {
        Serial.println("Connection failed");
        WiFi.disconnect();
        return false;
    }
}

// ==================== NVS 存儲 ====================
bool loadCredentials() {
    preferences.begin(NVS_NAMESPACE, true);
    savedSSID = preferences.getString(KEY_SSID, "");
    savedPass = preferences.getString(KEY_PASS, "");
    isConfigured = preferences.getBool(KEY_CONFIGURED, false);
    preferences.end();
    
    if (isConfigured && savedSSID.length() > 0) {
        Serial.printf("Loaded credentials for: %s\n", savedSSID.c_str());
        return true;
    }
    return false;
}

void saveCredentials(const char* ssid, const char* pass) {
    preferences.begin(NVS_NAMESPACE, false);
    preferences.putString(KEY_SSID, ssid);
    preferences.putString(KEY_PASS, pass);
    preferences.putBool(KEY_CONFIGURED, true);
    preferences.end();
    
    savedSSID = ssid;
    savedPass = pass;
    isConfigured = true;
    
    Serial.printf("Saved credentials for: %s\n", ssid);
}

void resetAllSettings() {
    preferences.begin(NVS_NAMESPACE, false);
    preferences.remove(KEY_SSID);
    preferences.remove(KEY_PASS);
    preferences.remove(KEY_CONFIGURED);
    preferences.end();
    
    savedSSID = "";
    savedPass = "";
    isConfigured = false;
    memset(currentInput, 0, sizeof(currentInput));
    inputLength = 0;
    passCursor = 0;
    tempChar = '1';
    
    WiFi.disconnect();
    
    Serial.println("All settings reset");
}

// ==================== 顯示畫面 ====================
void drawInitScreen() {
    u8g2.clearBuffer();
//    u8g2.setFont(u8g2_font_5x8_tr);
    u8g2.setCursor(20, 30);
    u8g2.print("ESP32 WiFi");
    u8g2.setCursor(10, 50);
    u8g2.print("Initializing...");
    u8g2.sendBuffer();
}

void drawScanningScreen() {
    static int dots = 0;
    dots = (dots + 1) % 4;
    String loading = "Scanning";
    for (int i = 0; i < dots; i++) loading += ".";
    
    u8g2.clearBuffer();
//    u8g2.setFont(u8g2_font_5x8_tr);
    u8g2.setCursor(30, 35);
    u8g2.print(loading);
    u8g2.sendBuffer();
}

void drawSelectScreen() {
    u8g2.clearBuffer();
//    u8g2.setFont(u8g2_font_5x8_tr);
    
    u8g2.drawBox(0, 0, 128, 12);
    u8g2.setColorIndex(0);
    u8g2.drawStr(5, 10, "Select WiFi");
    u8g2.setColorIndex(1);
    
    int maxDisplay = min(scanCount, 5);
    for (int i = 0; i < maxDisplay; i++) {
        int y = 20 + i * 10;
        if (i == selectedIndex) {
            u8g2.drawStr(0, y, ">");
        }
        
        String displaySSID = scanResults[i];
        if (displaySSID.length() > 12) {
            displaySSID = displaySSID.substring(0, 12) + "..";
        }
        
        char buffer[20];
        displaySSID.toCharArray(buffer, sizeof(buffer));
        u8g2.drawStr(12, y, buffer);
        
        int bars = map(scanRSSI[i], -100, -30, 1, 4);
        bars = constrain(bars, 1, 4);
        int barX = 108;
        for (int b = 0; b < bars; b++) {
            u8g2.drawBox(barX + b * 4, y - 4, 3, 6 - b);
        }
    }
    
    if (selectedIndex == scanCount) {
//        u8g2.setFont(u8g2_font_5x8_tr);
        u8g2.drawStr(5, 72, "[Refresh]");
    } else {
//        u8g2.setFont(u8g2_font_5x8_tr);
        u8g2.drawStr(5, 72, "Click:Select Double:OK");
    }
    
    u8g2.sendBuffer();
}

void drawPasswordInput() {
    u8g2.clearBuffer();
//    u8g2.setFont(u8g2_font_5x8_tr);
    
    // 第1栏: SSID (y:0-10)
    u8g2.drawBox(0, 0, 128, 10);
    u8g2.setColorIndex(0);
    u8g2.drawStr(2, 8, "SSID:");
    u8g2.drawStr(28, 8, selectedSSID.c_str());
    u8g2.setColorIndex(1);
    
    // 第2栏: 密码输入区 (y:12-48)
    u8g2.setFont(u8g2_font_9x15_tr);
    u8g2.drawStr(2, 21, "password:");
    
    if (passCursor == OK_INDEX) {
        u8g2.drawStr(2, 39, currentInput);
        u8g2.drawStr(2 + strlen(currentInput) * 9, 39, "[OK]");
    } else {
        u8g2.drawStr(2, 39, currentInput);
        char curBuf[2] = {tempChar, 0};
        u8g2.drawStr(2 + inputLength * 9, 39, curBuf);
        u8g2.drawHLine(2 + inputLength * 9, 41, 9);
    }
    
    // 第3栏: 操作提示 (y:50-64)
    u8g2.setFont(u8g2_font_5x8_tr);
    u8g2.drawStr(2, 52, "1:Chg 2:OK 3:Del");
    char posBuf[12];
    sprintf(posBuf, "%d/%d", passCursor + 1, OK_INDEX + 1);
    u8g2.drawStr(95, 52, posBuf);
    u8g2.drawStr(2, 60, "L:Save+Exit");
    
    u8g2.sendBuffer();
}

void drawConnectingScreen() {
    u8g2.clearBuffer();
//    u8g2.setFont(u8g2_font_5x8_tr);
    
    u8g2.setCursor(30, 25);
    u8g2.print("Connecting");
    
    u8g2.setCursor(10, 45);
    String connSSID = selectedSSID;
    if (connSSID.length() > 14) connSSID = connSSID.substring(0, 14) + "..";
    char connBuffer[20];
    connSSID.toCharArray(connBuffer, sizeof(connBuffer));
    u8g2.drawStr(10, 45, connBuffer);
    
    u8g2.setCursor(30, 60);
    u8g2.print("Please wait");
    
    u8g2.sendBuffer();
}

void drawConnectedScreen() {
    u8g2.clearBuffer();
//    u8g2.setFont(u8g2_font_5x8_tr);
    
    u8g2.setCursor(25, 20);
    u8g2.print("Connected!");
    
    String connSSID = selectedSSID;
    if (connSSID.length() > 14) connSSID = connSSID.substring(0, 14) + "..";
    char connBuffer[20];
    connSSID.toCharArray(connBuffer, sizeof(connBuffer));
    u8g2.drawStr(5, 40, "SSID:");
    u8g2.drawStr(35, 40, connBuffer);
    
    u8g2.drawStr(5, 55, "Click for IP");
    
    u8g2.sendBuffer();
}

void drawFailedScreen() {
    u8g2.clearBuffer();
//    u8g2.setFont(u8g2_font_5x8_tr);
    
    u8g2.setCursor(30, 20);
    u8g2.print("Connection");
    u8g2.setCursor(30, 35);
    u8g2.print("Failed!");
    
    u8g2.drawStr(5, 55, "Click: Try again");
    u8g2.drawStr(5, 68, "Hold 5s: Reset");
    
    u8g2.sendBuffer();
}

void drawConfirmReset() {
    u8g2.clearBuffer();
//    u8g2.setFont(u8g2_font_5x8_tr);
    
    u8g2.setCursor(20, 15);
    u8g2.print("Auto Connect");
    u8g2.setCursor(25, 30);
    u8g2.print("Failed!");
    
//    u8g2.setFont(u8g2_font_5x8_tr);
    u8g2.drawStr(5, 45, "Reset saved SSID?");
    
//    u8g2.setFont(u8g2_font_5x8_tr);
    u8g2.drawStr(5, 60, "1click: Reset");
    u8g2.drawStr(5, 72, "2click: Manual");
    
    u8g2.sendBuffer();
}

void drawShowIP() {
    u8g2.clearBuffer();
//    u8g2.setFont(u8g2_font_5x8_tr);
    
    u8g2.drawBox(0, 0, 128, 12);
    u8g2.setColorIndex(0);
    u8g2.drawStr(5, 10, "WiFi Info");
    u8g2.setColorIndex(1);
    
    u8g2.drawStr(0, 20, "SSID:");
    u8g2.drawStr(30, 20, selectedSSID.c_str());
    
    u8g2.drawStr(0, 35, "IP:");
    u8g2.drawStr(15, 35, WiFi.localIP().toString().c_str());
    
    u8g2.drawStr(0, 50, "RSSI:");
    char rssiBuffer[10];
    sprintf(rssiBuffer, "%d dBm", WiFi.RSSI());
    u8g2.drawStr(35, 50, rssiBuffer);
    
    u8g2.drawStr(5, 70, "Any key to back");
    
    u8g2.sendBuffer();
}
