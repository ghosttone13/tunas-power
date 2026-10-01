#include <Arduino.h>
#include <ModbusRTU.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <WiFiManager.h>

// ======================================================
// PIN
// ======================================================

#define RXD2       18
#define TXD2       17
#define RS485_DE   21
#define BTN        4
#define LED        3

// ======================================================
// RS485 / MODBUS
// ======================================================

HardwareSerial RS485Serial(2);
ModbusRTU mb;

// ======================================================
// DATA
// ======================================================

struct PZEMData
{
    float voltage;
    float current;
    float power;
    float energy;
    float frequency;
};

struct MD02Data
{
    float temperature;
    float humidity;
};

PZEMData pzem[12];
MD02Data md02[4];

// ======================================================
// MQTT
// ======================================================

const char *mqttServer = "mqtt.kampungiot.com";
const uint16_t mqttPort = 1883;

const char *mqttUser = "mqttuser";
const char *mqttPassword = "Robotika1";

const char *mqttTopic = "tunas/gresik";

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

// ======================================================
// WIFI MANAGER
// ======================================================

WiFiManager wm;

bool wifiManagerMode = false;

// AP WiFiManager
const char *AP_NAME = "TUNAS-POWER-SETUP";

// timeout portal 3 menit
const unsigned long WIFI_PORTAL_TIMEOUT = 180000UL;

// ======================================================
// BUTTON
// ======================================================

bool btnLastState = HIGH;
unsigned long btnPressStart = 0;

bool btnLongPressTriggered = false;

const unsigned long BTN_LONG_PRESS_TIME = 5000UL;

// ======================================================
// LED STATUS
// ======================================================

unsigned long lastLedMillis = 0;
bool ledState = false;

const unsigned long LED_FAST_INTERVAL = 100;    // WiFiManager
const unsigned long LED_SLOW_INTERVAL = 1000;   // WiFi connected

// ======================================================
// MODBUS
// ======================================================

uint8_t currentID = 1;

bool requestRunning = false;

uint16_t pzemReg[10];
uint16_t md02Reg[2];

unsigned long lastRequest = 0;

const uint16_t REQUEST_INTERVAL = 250;

// ======================================================
// WIFI CONNECTION
// ======================================================

void startWiFiManager()
{
    Serial.println();
    Serial.println("==============================");
    Serial.println(" MASUK WIFI MANAGER");
    Serial.println("==============================");

    wifiManagerMode = true;

    // --------------------------------------------------
    // PUTUS KONEKSI WIFI LAMA
    // --------------------------------------------------

    WiFi.disconnect(false, false);

    // Putus MQTT
    if (mqtt.connected())
    {
        mqtt.disconnect();
    }

    // --------------------------------------------------
    // WIFI MANAGER NON-BLOCKING
    // --------------------------------------------------

    wm.setConfigPortalBlocking(false);

    // Timeout 3 menit
    wm.setConfigPortalTimeout(180);

    // Mulai Config Portal
    wm.startConfigPortal(AP_NAME);

    Serial.println("WiFiManager aktif!");
    Serial.println("SSID : TUNAS-POWER-SETUP");
    Serial.println("IP   : 192.168.4.1");

    // --------------------------------------------------
    // RESET LED
    // --------------------------------------------------

    lastLedMillis = millis();
    ledState = false;

    digitalWrite(LED, LOW);
}
// ======================================================
// CHECK WIFI MANAGER
// ======================================================

void handleWiFiManager()
{
    if (!wifiManagerMode)
        return;

    // Proses WiFiManager
    wm.process();

    // ==================================================
    // LED KEDIP CEPAT 100ms
    // ==================================================

    unsigned long now = millis();

    if (now - lastLedMillis >= LED_FAST_INTERVAL)
    {
        lastLedMillis = now;

        ledState = !ledState;

        digitalWrite(
            LED,
            ledState ? HIGH : LOW
        );
    }

    // ==================================================
    // WIFI BERHASIL TERHUBUNG
    // ==================================================

    if (WiFi.status() == WL_CONNECTED)
    {
        Serial.println();
        Serial.println("==============================");
        Serial.println(" WIFI BERHASIL TERHUBUNG");
        Serial.println("==============================");

        Serial.print("SSID : ");
        Serial.println(WiFi.SSID());

        Serial.print("IP   : ");
        Serial.println(WiFi.localIP());

        // Keluar dari WiFiManager
        wifiManagerMode = false;

        wm.stopConfigPortal();

        // Reset LED
        ledState = false;
        digitalWrite(LED, LOW);

        lastLedMillis = millis();

        // MQTT
        mqtt.setServer(
            mqttServer,
            mqttPort
        );

        return;
    }
}
// ======================================================
// WIFI NORMAL
// ======================================================

void connectWiFi()
{
    if (WiFi.status() == WL_CONNECTED)
        return;

    Serial.println("Mencoba koneksi WiFi...");

    WiFi.mode(WIFI_STA);

    // Gunakan konfigurasi yang tersimpan
    WiFi.begin();

    unsigned long start = millis();

    while (WiFi.status() != WL_CONNECTED &&
           millis() - start < 10000)
    {
        delay(100);

        Serial.print(".");
    }

    Serial.println();

    if (WiFi.status() == WL_CONNECTED)
    {
        Serial.println("WiFi Connected");
        Serial.print("IP Address: ");
        Serial.println(WiFi.localIP());
    }
    else
    {
        Serial.println("WiFi belum terhubung.");
    }
}

// ======================================================
// LED STATUS NORMAL
// ======================================================

void handleNormalLED()
{
    // Jangan proses kalau sedang WiFiManager
    if (wifiManagerMode)
        return;

    unsigned long now = millis();

    // --------------------------------------------------
    // WIFI CONNECTED
    // LED KEDIP 1000ms
    // --------------------------------------------------

    if (WiFi.status() == WL_CONNECTED)
    {
        if (now - lastLedMillis >= LED_SLOW_INTERVAL)
        {
            lastLedMillis = now;

            ledState = !ledState;

            digitalWrite(
                LED,
                ledState ? HIGH : LOW
            );
        }
    }

    // --------------------------------------------------
    // WIFI TIDAK TERHUBUNG
    // LED MATI
    // --------------------------------------------------

    else
    {
        ledState = false;
        digitalWrite(LED, LOW);

        lastLedMillis = now;
    }
}

// ======================================================
// BUTTON
// ======================================================

void handleButton()
{
    bool currentState = digitalRead(BTN);

    // Tombol baru ditekan
    if (currentState == LOW && btnLastState == HIGH)
    {
        btnPressStart = millis();
        btnLongPressTriggered = false;

        Serial.println("BTN ditekan");
    }

    // Tombol masih ditekan
    if (currentState == LOW)
    {
        if (!btnLongPressTriggered &&
            millis() - btnPressStart >= BTN_LONG_PRESS_TIME)
        {
            btnLongPressTriggered = true;

            Serial.println("BTN ditahan 5 detik");

            if (!wifiManagerMode)
            {
                startWiFiManager();
            }
        }
    }

    // Tombol dilepas
    if (currentState == HIGH && btnLastState == LOW)
    {
        Serial.println("BTN dilepas");
    }

    btnLastState = currentState;
}

// ======================================================
// MQTT
// ======================================================

void connectMQTT()
{
    // Tidak ada WiFi
    if (WiFi.status() != WL_CONNECTED)
        return;

    // Sudah terhubung
    if (mqtt.connected())
        return;

    // Jangan connect saat WiFiManager
    if (wifiManagerMode)
        return;

    static unsigned long lastMQTTAttempt = 0;

    const unsigned long MQTT_RETRY_INTERVAL = 5000;

    unsigned long now = millis();

    // Tunggu 5 detik sebelum mencoba lagi
    if (now - lastMQTTAttempt < MQTT_RETRY_INTERVAL)
        return;

    lastMQTTAttempt = now;

    Serial.print("MQTT...");

    if (mqtt.connect(
            "ESP32_POWER",
            mqttUser,
            mqttPassword))
    {
        Serial.println("connected");

        // Reset timer
        lastMQTTAttempt = millis();
    }
    else
    {
        Serial.printf(
            "failed rc=%d\n",
            mqtt.state()
        );
    }
}
// ======================================================
// PUBLISH MQTT
// ======================================================

void publishMQTT()
{
    if (!mqtt.connected())
        return;

    StaticJsonDocument<4096> doc;

    doc["device_id"] = "TUNAS-POWER";
    doc["hierarchy_code"] = "01.L01.Z01";

    // --------------------------------------------------
    // MD02
    // --------------------------------------------------

    for (int i = 0; i < 4; i++)
    {
        doc["temperature_" + String(i + 1)] =
            md02[i].temperature;

        doc["humidity_" + String(i + 1)] =
            md02[i].humidity;
    }

    // --------------------------------------------------
    // PZEM
    // --------------------------------------------------

    for (int i = 0; i < 12; i++)
    {
        doc["voltage_" + String(i + 1)] =
            pzem[i].voltage;

        doc["current_" + String(i + 1)] =
            pzem[i].current;

        doc["power_" + String(i + 1)] =
            pzem[i].power;

        doc["energy_" + String(i + 1)] =
            pzem[i].energy;

        doc["frequency_" + String(i + 1)] =
            pzem[i].frequency;
    }

    char payload[4096];

    size_t len = serializeJson(
        doc,
        payload
    );

    bool ok = mqtt.publish(
        mqttTopic,
        payload,
        len
    );

    Serial.print("Publish : ");
    Serial.println(
        ok ? "SUCCESS" : "FAILED"
    );
}

// ======================================================
// MODBUS CALLBACK
// ======================================================

bool cb(
    Modbus::ResultCode event,
    uint16_t transactionId,
    void *data)
{
    if (event == Modbus::EX_SUCCESS)
    {
        // ------------------------------------------------
        // PZEM
        // ------------------------------------------------

        if (currentID <= 12)
        {
            uint8_t i = currentID - 1;

            pzem[i].voltage =
                pzemReg[0] / 10.0;

            uint32_t c =
                ((uint32_t)pzemReg[1] << 16) |
                pzemReg[2];

            uint32_t p =
                ((uint32_t)pzemReg[3] << 16) |
                pzemReg[4];

            uint32_t e =
                ((uint32_t)pzemReg[5] << 16) |
                pzemReg[6];

            pzem[i].current =
                c / 1000.0;

            pzem[i].power =
                p / 10.0;

            pzem[i].energy =
                e / 1000.0;

            pzem[i].frequency =
                pzemReg[7] / 10.0;
        }

        // ------------------------------------------------
        // MD02
        // ------------------------------------------------

        else
        {
            uint8_t i = currentID - 13;

            md02[i].temperature =
                md02Reg[0] / 10.0;

            md02[i].humidity =
                md02Reg[1] / 10.0;
        }
    }

    requestRunning = false;

    currentID++;

    if (currentID > 16)
    {
        currentID = 1;

        publishMQTT();
    }

    return true;
}

// ======================================================
// MODBUS
// ======================================================

void handleModbus()
{
    // Jangan baca Modbus saat WiFiManager
    if (wifiManagerMode)
        return;

    mb.task();

    if (requestRunning)
        return;

    if (millis() - lastRequest < REQUEST_INTERVAL)
        return;

    lastRequest = millis();

    bool ok = false;

    // --------------------------------------------------
    // PZEM ID 1 - 12
    // --------------------------------------------------

    if (currentID <= 12)
    {
        ok = mb.readIreg(
            currentID,
            0x0000,
            pzemReg,
            9,
            cb
        );
    }

    // --------------------------------------------------
    // MD02 ID 13 - 16
    // --------------------------------------------------

    else
    {
        ok = mb.readIreg(
            currentID,
            0x0001,
            md02Reg,
            2,
            cb
        );
    }

    if (ok)
    {
        requestRunning = true;
    }
}

// ======================================================
// SETUP
// ======================================================

void setup()
{
    Serial.begin(115200);

    // --------------------------------------------------
    // PIN
    // --------------------------------------------------

    pinMode(BTN, INPUT_PULLUP);

    pinMode(LED, OUTPUT);
    digitalWrite(LED, LOW);

    pinMode(RS485_DE, OUTPUT);
    digitalWrite(RS485_DE, LOW);

    // --------------------------------------------------
    // WIFI
    // --------------------------------------------------

    WiFi.mode(WIFI_STA);

    connectWiFi();

    // --------------------------------------------------
    // MQTT
    // --------------------------------------------------

    mqtt.setServer(
        mqttServer,
        mqttPort
    );

    mqtt.setBufferSize(4096);

    if (WiFi.status() == WL_CONNECTED)
    {
        connectMQTT();
    }

    // --------------------------------------------------
    // RS485
    // --------------------------------------------------

    RS485Serial.begin(
        9600,
        SERIAL_8N1,
        RXD2,
        TXD2
    );

    mb.begin(
        &RS485Serial,
        RS485_DE
    );

    mb.master();

    Serial.println();
    Serial.println("==============================");
    Serial.println(" TUNAS POWER MONITOR");
    Serial.println("==============================");
}

// ======================================================
// LOOP
// ======================================================

void loop()
{
    // --------------------------------------------------
    // BUTTON
    // --------------------------------------------------

    handleButton();

    // --------------------------------------------------
    // WIFI MANAGER
    // --------------------------------------------------

    if (wifiManagerMode)
    {
        handleWiFiManager();

        // Jangan jalankan MQTT/Modbus
        // selama konfigurasi WiFi
        return;
    }

    // --------------------------------------------------
    // WIFI
    // --------------------------------------------------

    if (WiFi.status() != WL_CONNECTED)
    {
        // Jangan blocking terlalu lama
        static unsigned long lastWiFiReconnect = 0;

        if (millis() - lastWiFiReconnect >= 10000)
        {
            lastWiFiReconnect = millis();

            connectWiFi();
        }
    }

    // --------------------------------------------------
    // LED STATUS
    // --------------------------------------------------

    handleNormalLED();

    // --------------------------------------------------
    // MQTT
    // --------------------------------------------------

    if (WiFi.status() == WL_CONNECTED)
    {
        if (!mqtt.connected())
        {
            connectMQTT();
        }

        mqtt.loop();
    }

    // --------------------------------------------------
    // MODBUS
    // --------------------------------------------------

    handleModbus();
}