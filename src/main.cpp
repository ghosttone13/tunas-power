#include <Arduino.h>
#include <ModbusRTU.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WiFiManager.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Update.h>

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

// ======================================================
// MQTT CONFIGURATION
// ======================================================
// Nilai credential MQTT TIDAK disimpan di source code.
// Credential disimpan di NVS ESP32 dan dapat diatur
// melalui halaman WiFiManager.
// ======================================================

String mqttServer = "mqtt.kampungiot.com";
uint16_t mqttPort = 1883;
String mqttUser = "";
String mqttPassword = "";
String mqttTopic = "tunas/gresik";
String mqttStatusTopic = "tunas/gresik/status";

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

// ======================================================
// OTA GITHUB
// ======================================================

// Firmware OTA diambil dari repository GitHub Public.
const char *FIRMWARE_URL =
    "https://raw.githubusercontent.com/ghosttone13/tunas-power/main/firmware/TUNAS-POWER.bin";

// Perintah OTA diterima MQTT, tetapi OTA dijalankan dari loop()
// agar mqttCallback() tidak terblokir oleh proses download.
bool otaRequested = false;
bool otaRunning = false;

// ======================================================
// WIFI MANAGER
// ======================================================

WiFiManager wm;
Preferences preferences;

// Field tambahan pada WiFiManager untuk konfigurasi MQTT.
char mqttServerParam[96] = "mqtt.kampungiot.com";
char mqttPortParam[8] = "1883";
char mqttUserParam[64] = "";
char mqttPasswordParam[64] = "";
char mqttTopicParam[128] = "tunas/gresik";

WiFiManagerParameter custom_mqtt_title(
    "<h3>MQTT Configuration</h3>"
);

WiFiManagerParameter custom_mqtt_server(
    "mqtt_server",
    "MQTT Server",
    mqttServerParam,
    sizeof(mqttServerParam)
);

WiFiManagerParameter custom_mqtt_port(
    "mqtt_port",
    "MQTT Port",
    mqttPortParam,
    sizeof(mqttPortParam)
);

WiFiManagerParameter custom_mqtt_user(
    "mqtt_user",
    "MQTT Username",
    mqttUserParam,
    sizeof(mqttUserParam)
);

WiFiManagerParameter custom_mqtt_password(
    "mqtt_password",
    "MQTT Password",
    mqttPasswordParam,
    sizeof(mqttPasswordParam),
    "type='password'"
);

WiFiManagerParameter custom_mqtt_topic(
    "mqtt_topic",
    "MQTT Topic",
    mqttTopicParam,
    sizeof(mqttTopicParam)
);

bool wifiManagerMode = false;

const char *AP_NAME = "TUNAS-POWER-SETUP";

// 3 menit
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
// FUNCTION PROTOTYPES
// ======================================================

void loadMQTTConfig();
void saveMQTTConfigFromWiFiManager();

void loadMQTTConfig()
{
    preferences.begin("mqttcfg", true);

    mqttServer = preferences.getString(
        "server",
        "mqtt.kampungiot.com"
    );

    mqttPort = (uint16_t)preferences.getUInt(
        "port",
        1883
    );

    mqttUser = preferences.getString("user", "");
    mqttPassword = preferences.getString("pass", "");
    mqttTopic = preferences.getString(
        "topic",
        "tunas/gresik"
    );

    preferences.end();

    mqttStatusTopic =
        mqttTopic + "/status";

    mqttServer.toCharArray(
        mqttServerParam,
        sizeof(mqttServerParam)
    );

    String portString = String(mqttPort);
    portString.toCharArray(
        mqttPortParam,
        sizeof(mqttPortParam)
    );

    mqttUser.toCharArray(
        mqttUserParam,
        sizeof(mqttUserParam)
    );

    mqttPassword.toCharArray(
        mqttPasswordParam,
        sizeof(mqttPasswordParam)
    );

    mqttTopic.toCharArray(
        mqttTopicParam,
        sizeof(mqttTopicParam)
    );

    Serial.println("MQTT config loaded from NVS");
    Serial.print("MQTT Server : ");
    Serial.println(mqttServer);
    Serial.print("MQTT Port   : ");
    Serial.println(mqttPort);
    Serial.print("MQTT User   : ");
    Serial.println(
        mqttUser.length() ? "(tersimpan)" : "(kosong)"
    );
    Serial.print("MQTT Topic  : ");
    Serial.println(mqttTopic);
}

void saveMQTTConfigFromWiFiManager()
{
    String newServer =
        String(custom_mqtt_server.getValue());
    String newPort =
        String(custom_mqtt_port.getValue());
    String newUser =
        String(custom_mqtt_user.getValue());
    String newPassword =
        String(custom_mqtt_password.getValue());
    String newTopic =
        String(custom_mqtt_topic.getValue());

    newServer.trim();
    newPort.trim();
    newUser.trim();
    newTopic.trim();

    if (newServer.length() == 0)
        newServer = "mqtt.kampungiot.com";

    uint16_t newPortValue =
        (uint16_t)newPort.toInt();

    if (newPortValue == 0)
        newPortValue = 1883;

    if (newTopic.length() == 0)
        newTopic = "tunas/gresik";

    // Password yang sudah tersimpan dipertahankan bila field
    // password dikosongkan pada portal.
    if (
        newPassword.length() == 0 &&
        mqttPassword.length() > 0
    )
    {
        newPassword = mqttPassword;
    }

    preferences.begin("mqttcfg", false);

    preferences.putString("server", newServer);
    preferences.putUInt("port", newPortValue);
    preferences.putString("user", newUser);
    preferences.putString("pass", newPassword);
    preferences.putString("topic", newTopic);

    preferences.end();

    mqttServer = newServer;
    mqttPort = newPortValue;
    mqttUser = newUser;
    mqttPassword = newPassword;
    mqttTopic = newTopic;
    mqttStatusTopic = mqttTopic + "/status";

    Serial.println("MQTT config saved to NVS");
}

void startWiFiManager();
void handleWiFiManager();

void connectWiFi();
void handleNormalLED();

void handleButton();

void mqttCallback(
    char *topic,
    byte *payload,
    unsigned int length
);

void connectMQTT();
void publishMQTT();

void publishOTAStatus(const char *status);
void publishOTAProgress(int progress);
bool performOTA();

bool cb(
    Modbus::ResultCode event,
    uint16_t transactionId,
    void *data
);

void handleModbus();

// ======================================================
// OTA STATUS
// ======================================================

void publishOTAStatus(const char *status)
{
    if (!mqtt.connected())
        return;

    StaticJsonDocument<256> doc;

    doc["device_id"] = "TUNAS-POWER";
    doc["ota"] = status;

    char payload[256];

    size_t len = serializeJson(
        doc,
        payload,
        sizeof(payload)
    );

    mqtt.publish(
        mqttStatusTopic.c_str(),
        payload,
        len
    );

    // Beri kesempatan PubSubClient memproses paket.
    mqtt.loop();

    Serial.print("OTA status: ");
    Serial.println(payload);
}

// ======================================================
// OTA PROGRESS
// ======================================================

void publishOTAProgress(int progress)
{
    if (!mqtt.connected())
        return;

    StaticJsonDocument<256> doc;

    doc["device_id"] = "TUNAS-POWER";
    doc["ota"] = "downloading";
    doc["progress"] = progress;

    char payload[256];

    size_t len = serializeJson(
        doc,
        payload,
        sizeof(payload)
    );

    mqtt.publish(
        mqttStatusTopic.c_str(),
        payload,
        len
    );

    mqtt.loop();
}

// ======================================================
// OTA GITHUB
// ======================================================

bool performOTA()
{
    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.println("OTA gagal: WiFi tidak terhubung.");
        return false;
    }

    Serial.println();
    Serial.println("==============================");
    Serial.println(" MEMULAI OTA DARI GITHUB");
    Serial.println("==============================");

    Serial.print("URL: ");
    Serial.println(FIRMWARE_URL);

    publishOTAStatus("started");

    WiFiClientSecure client;

    // Untuk pengujian GitHub.
    // Untuk produksi sebaiknya gunakan CA certificate
    // dan/atau signature verification firmware.
    client.setInsecure();

    HTTPClient http;

    http.setFollowRedirects(
        HTTPC_STRICT_FOLLOW_REDIRECTS
    );

    http.setTimeout(15000);

    if (!http.begin(client, FIRMWARE_URL))
    {
        Serial.println("OTA gagal: HTTP begin gagal.");
        publishOTAStatus("failed_http_begin");
        return false;
    }

    // Hindari firmware lama dari cache.
    http.addHeader(
        "Cache-Control",
        "no-cache"
    );

    int httpCode = http.GET();

    Serial.print("HTTP code: ");
    Serial.println(httpCode);

    if (httpCode != HTTP_CODE_OK)
    {
        Serial.print("OTA gagal, HTTP code = ");
        Serial.println(httpCode);

        http.end();

        publishOTAStatus("failed_http");

        return false;
    }

    int contentLength = http.getSize();

    Serial.print("Firmware size: ");
    Serial.println(contentLength);

    if (contentLength <= 0)
    {
        Serial.println(
            "OTA gagal: ukuran firmware tidak valid."
        );

        http.end();

        publishOTAStatus("failed_size");

        return false;
    }

    if (!Update.begin(
            (size_t)contentLength,
            U_FLASH))
    {
        Serial.print(
            "Update.begin gagal: "
        );

        Serial.println(
            Update.errorString()
        );

        http.end();

        publishOTAStatus("failed_update_begin");

        return false;
    }

    WiFiClient *stream =
        http.getStreamPtr();

    size_t written = 0;

    int lastProgress = -1;

    unsigned long lastDataTime = millis();

    uint8_t buffer[1024];

    while (
        http.connected() &&
        written < (size_t)contentLength)
    {
        size_t available =
            stream->available();

        if (available > 0)
        {
            size_t readSize =
                available;

            if (readSize >
                sizeof(buffer))
            {
                readSize =
                    sizeof(buffer);
            }

            size_t readBytes =
                stream->readBytes(
                    buffer,
                    readSize
                );

            if (readBytes > 0)
            {
                size_t writtenNow =
                    Update.write(
                        buffer,
                        readBytes
                    );

                if (writtenNow !=
                    readBytes)
                {
                    Serial.println(
                        "OTA gagal: "
                        "penulisan flash gagal."
                    );

                    Update.abort();

                    http.end();

                    publishOTAStatus(
                        "failed_write"
                    );

                    return false;
                }

                written += writtenNow;

                lastDataTime = millis();

                int progress =
                    (int)(
                        (written * 100ULL) /
                        (size_t)contentLength
                    );

                if (
                    progress != lastProgress &&
                    (
                        progress % 5 == 0 ||
                        progress == 100
                    )
                )
                {
                    lastProgress =
                        progress;

                    Serial.print(
                        "OTA progress: "
                    );

                    Serial.print(
                        progress
                    );

                    Serial.println("%");

                    publishOTAProgress(
                        progress
                    );
                }
            }
        }
        else
        {
            if (
                millis() - lastDataTime >
                15000UL)
            {
                Serial.println(
                    "OTA gagal: "
                    "timeout download."
                );

                Update.abort();

                http.end();

                publishOTAStatus(
                    "failed_timeout"
                );

                return false;
            }

            delay(1);
            yield();
        }
    }

    http.end();

    if (
        written !=
        (size_t)contentLength)
    {
        Serial.println(
            "OTA gagal: "
            "ukuran data tidak sesuai."
        );

        Update.abort();

        publishOTAStatus(
            "failed_incomplete"
        );

        return false;
    }

    if (!Update.end())
    {
        Serial.print(
            "Update.end gagal: "
        );

        Serial.println(
            Update.errorString()
        );

        publishOTAStatus(
            "failed_update_end"
        );

        return false;
    }

    if (!Update.isFinished())
    {
        Serial.println(
            "OTA gagal: "
            "update belum selesai."
        );

        publishOTAStatus(
            "failed_not_finished"
        );

        return false;
    }

    Serial.println();
    Serial.println("==============================");
    Serial.println(" OTA BERHASIL");
    Serial.println(" ESP32 AKAN RESTART");
    Serial.println("==============================");

    publishOTAStatus(
        "success"
    );

    delay(500);

    ESP.restart();

    return true;
}

// ======================================================
// WIFI MANAGER
// ======================================================

void startWiFiManager()
{
    Serial.println();
    Serial.println("==============================");
    Serial.println(" MASUK WIFI MANAGER");
    Serial.println("==============================");

    wifiManagerMode = true;

    // Hentikan MQTT.
    if (mqtt.connected())
    {
        mqtt.disconnect();
    }

    // Penting:
    // Putus koneksi WiFi lama supaya WiFiManager tidak langsung
    // menganggap ESP32 sudah terhubung dan keluar dari portal.
    WiFi.disconnect(false, false);

    WiFi.mode(WIFI_STA);

    // WiFiManager non-blocking.
    wm.setConfigPortalBlocking(false);

    // Timeout portal 3 menit.
    wm.setConfigPortalTimeout(180);

    // Tambahkan konfigurasi MQTT ke halaman WiFiManager.
    wm.addParameter(&custom_mqtt_title);
    wm.addParameter(&custom_mqtt_server);
    wm.addParameter(&custom_mqtt_port);
    wm.addParameter(&custom_mqtt_user);
    wm.addParameter(&custom_mqtt_password);
    wm.addParameter(&custom_mqtt_topic);

    // Karena WiFiManager dijalankan dalam mode NON-BLOCKING,
    // nilai return dari startConfigPortal() tidak boleh dianggap
    // sebagai tanda bahwa portal gagal.
    //
    // Pada mode non-blocking, fungsi ini hanya MEMULAI portal,
    // kemudian proses selanjutnya dilakukan oleh wm.process()
    // di loop().
    wm.startConfigPortal(
        AP_NAME
    );

    Serial.println(
        "WiFiManager aktif!"
    );

    Serial.println(
        "SSID : TUNAS-POWER-SETUP"
    );

    Serial.println(
        "IP   : 192.168.4.1"
    );

    lastLedMillis = millis();

    ledState = false;

    digitalWrite(
        LED,
        LOW
    );
}

// ======================================================
// CHECK WIFI MANAGER
// ======================================================

void handleWiFiManager()
{
    if (!wifiManagerMode)
        return;

    // Proses WiFiManager.
    wm.process();

    // LED kedip cepat 100 ms.
    unsigned long now =
        millis();

    if (
        now - lastLedMillis >=
        LED_FAST_INTERVAL)
    {
        lastLedMillis = now;

        ledState = !ledState;

        digitalWrite(
            LED,
            ledState ? HIGH : LOW
        );
    }

    // WiFi berhasil terhubung.
    if (
        WiFi.status() ==
        WL_CONNECTED)
    {
        Serial.println();
        Serial.println("==============================");
        Serial.println(
            " WIFI BERHASIL TERHUBUNG"
        );
        Serial.println("==============================");

        Serial.print(
            "SSID : "
        );

        Serial.println(
            WiFi.SSID()
        );

        Serial.print(
            "IP   : "
        );

        Serial.println(
            WiFi.localIP()
        );

        // Simpan konfigurasi MQTT dari form WiFiManager.
        saveMQTTConfigFromWiFiManager();

        wifiManagerMode =
            false;

        wm.stopConfigPortal();

        ledState = false;

        digitalWrite(
            LED,
            LOW
        );

        lastLedMillis =
            millis();

        mqtt.setServer(
            mqttServer.c_str(),
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
    if (
        WiFi.status() ==
        WL_CONNECTED)
    {
        return;
    }

    Serial.println(
        "Mencoba koneksi WiFi..."
    );

    WiFi.mode(
        WIFI_STA
    );

    // Gunakan konfigurasi WiFi yang
    // tersimpan di ESP32/WiFiManager.
    WiFi.begin();

    unsigned long start =
        millis();

    while (
        WiFi.status() !=
            WL_CONNECTED &&
        millis() - start <
            10000UL)
    {
        delay(100);

        Serial.print(".");
    }

    Serial.println();

    if (
        WiFi.status() ==
        WL_CONNECTED)
    {
        Serial.println(
            "WiFi Connected"
        );

        Serial.print(
            "IP Address: "
        );

        Serial.println(
            WiFi.localIP()
        );
    }
    else
    {
        Serial.println(
            "WiFi belum terhubung."
        );
    }
}

// ======================================================
// LED STATUS NORMAL
// ======================================================

void handleNormalLED()
{
    if (wifiManagerMode)
        return;

    unsigned long now =
        millis();

    // ==================================================
    // WIFI TERHUBUNG
    // LED KEDIP 1000 ms
    // ==================================================

    if (
        WiFi.status() ==
        WL_CONNECTED)
    {
        if (
            now - lastLedMillis >=
            LED_SLOW_INTERVAL)
        {
            lastLedMillis =
                now;

            ledState =
                !ledState;

            digitalWrite(
                LED,
                ledState ?
                    HIGH :
                    LOW
            );
        }
    }

    // ==================================================
    // WIFI TIDAK TERHUBUNG
    // LED MATI
    // ==================================================

    else
    {
        ledState =
            false;

        digitalWrite(
            LED,
            LOW
        );

        lastLedMillis =
            now;
    }
}

// ======================================================
// BUTTON
// ======================================================

void handleButton()
{
    bool currentState =
        digitalRead(BTN);

    // Tombol baru ditekan.
    if (
        currentState == LOW &&
        btnLastState == HIGH)
    {
        btnPressStart =
            millis();

        btnLongPressTriggered =
            false;

        Serial.println(
            "BTN ditekan"
        );
    }

    // Tombol masih ditekan.
    if (
        currentState == LOW)
    {
        if (
            !btnLongPressTriggered &&
            millis() -
                btnPressStart >=
                BTN_LONG_PRESS_TIME)
        {
            btnLongPressTriggered =
                true;

            Serial.println(
                "BTN ditahan 5 detik"
            );

            if (!wifiManagerMode)
            {
                startWiFiManager();
            }
        }
    }

    // Tombol dilepas.
    if (
        currentState == HIGH &&
        btnLastState == LOW)
    {
        Serial.println(
            "BTN dilepas"
        );
    }

    btnLastState =
        currentState;
}

// ======================================================
// MQTT CALLBACK
// ======================================================

void mqttCallback(
    char *topic,
    byte *payload,
    unsigned int length)
{
    if (otaRunning)
        return;

    Serial.println();

    Serial.print(
        "MQTT message dari topic: "
    );

    Serial.println(
        topic
    );

    // Hanya proses topic utama.
    if (
        String(topic) !=
        mqttTopic)
    {
        return;
    }

    StaticJsonDocument<512>
        doc;

    DeserializationError error =
        deserializeJson(
            doc,
            payload,
            length
        );

    if (error)
    {
        Serial.print(
            "JSON error: "
        );

        Serial.println(
            error.c_str()
        );

        return;
    }

    // Perintah OTA:
    // {"update":1}
    if (
        doc["update"].is<int>() &&
        doc["update"].as<int>() == 1)
    {
        if (!otaRequested)
        {
            Serial.println(
                "Perintah OTA diterima: "
                "update = 1"
            );

            // Jangan menjalankan OTA langsung
            // dari callback MQTT.
            otaRequested = true;

            // Hentikan transaksi Modbus
            // yang sedang menunggu.
            requestRunning = false;
        }
        else
        {
            Serial.println(
                "OTA sudah menunggu proses."
            );
        }
    }
}

// ======================================================
// MQTT CONNECTION
// ======================================================

void connectMQTT()
{
    if (
        WiFi.status() !=
        WL_CONNECTED)
    {
        return;
    }

    if (mqtt.connected())
        return;

    if (
        wifiManagerMode ||
        otaRunning)
    {
        return;
    }

    static unsigned long
        lastMQTTAttempt = 0;

    const unsigned long
        MQTT_RETRY_INTERVAL =
            5000UL;

    unsigned long now =
        millis();

    // Non-blocking:
    // tidak memakai while() atau delay(3000).
    if (
        now - lastMQTTAttempt <
        MQTT_RETRY_INTERVAL)
    {
        return;
    }

    lastMQTTAttempt =
        now;

    mqtt.setServer(
        mqttServer.c_str(),
        mqttPort
    );

    mqtt.setCallback(
        mqttCallback
    );

    Serial.print(
        "MQTT..."
    );

    bool connected;

    // MQTT tanpa username/password.
    if (
        mqttUser[0] == '\0' &&
        mqttPassword[0] == '\0')
    {
        connected =
            mqtt.connect(
                "ESP32_POWER"
            );
    }
    else
    {
        connected =
            mqtt.connect(
                "ESP32_POWER",
                mqttUser.c_str(),
                mqttPassword.c_str()
            );
    }

    if (connected)
    {
        Serial.println(
            "connected"
        );

        mqtt.subscribe(
            mqttTopic.c_str()
        );

        Serial.print(
            "Subscribe: "
        );

        Serial.println(
            mqttTopic
        );
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

    StaticJsonDocument<4096>
        doc;

    doc["device_id"] =
        "TUNAS-POWER";

    doc["hierarchy_code"] =
        "01.L01.Z01";

    // ==================================================
    // MD02
    // ==================================================

    for (int i = 0; i < 4; i++)
    {
        doc[
            "temperature_" +
            String(i + 1)
        ] =
            md02[i].temperature;

        doc[
            "humidity_" +
            String(i + 1)
        ] =
            md02[i].humidity;
    }

    // ==================================================
    // PZEM
    // ==================================================

    for (int i = 0; i < 12; i++)
    {
        doc[
            "voltage_" +
            String(i + 1)
        ] =
            pzem[i].voltage;

        doc[
            "current_" +
            String(i + 1)
        ] =
            pzem[i].current;

        doc[
            "power_" +
            String(i + 1)
        ] =
            pzem[i].power;

        doc[
            "energy_" +
            String(i + 1)
        ] =
            pzem[i].energy;

        doc[
            "frequency_" +
            String(i + 1)
        ] =
            pzem[i].frequency;
    }

    char payload[4096];

    size_t len =
        serializeJson(
            doc,
            payload,
            sizeof(payload)
        );

    bool ok =
        mqtt.publish(
            mqttTopic.c_str(),
            payload,
            len
        );

    Serial.print(
        "Publish : "
    );

    Serial.println(
        ok ?
            "SUCCESS" :
            "FAILED"
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
    if (
        event ==
        Modbus::EX_SUCCESS)
    {
        // ==================================================
        // PZEM ID 1 - 12
        // ==================================================

        if (
            currentID <= 12)
        {
            uint8_t i =
                currentID - 1;

            pzem[i].voltage =
                pzemReg[0] /
                10.0;

            uint32_t c =
                (
                    (uint32_t)
                    pzemReg[1]
                    << 16
                ) |
                pzemReg[2];

            uint32_t p =
                (
                    (uint32_t)
                    pzemReg[3]
                    << 16
                ) |
                pzemReg[4];

            uint32_t e =
                (
                    (uint32_t)
                    pzemReg[5]
                    << 16
                ) |
                pzemReg[6];

            pzem[i].current =
                c / 1000.0;

            pzem[i].power =
                p / 10.0;

            pzem[i].energy =
                e / 1000.0;

            pzem[i].frequency =
                pzemReg[7] /
                10.0;
        }

        // ==================================================
        // MD02 ID 13 - 16
        // ==================================================

        else
        {
            uint8_t i =
                currentID - 13;

            md02[i].temperature =
                md02Reg[0] /
                10.0;

            md02[i].humidity =
                md02Reg[1] /
                10.0;
        }
    }

    requestRunning =
        false;

    currentID++;

    if (
        currentID > 16)
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
    // Jangan baca Modbus saat WiFiManager.
    if (wifiManagerMode)
        return;

    mb.task();

    if (requestRunning)
        return;

    if (
        millis() - lastRequest <
        REQUEST_INTERVAL)
    {
        return;
    }

    lastRequest =
        millis();

    bool ok = false;

    // ==================================================
    // PZEM ID 1 - 12
    // ==================================================

    if (
        currentID <= 12)
    {
        ok =
            mb.readIreg(
                currentID,
                0x0000,
                pzemReg,
                9,
                cb
            );
    }

    // ==================================================
    // MD02 ID 13 - 16
    // ==================================================

    else
    {
        ok =
            mb.readIreg(
                currentID,
                0x0001,
                md02Reg,
                2,
                cb
            );
    }

    if (ok)
    {
        requestRunning =
            true;
    }
}

// ======================================================
// SETUP
// ======================================================

void setup()
{
    Serial.begin(
        115200
    );

    // ==================================================
    // PIN
    // ==================================================

    pinMode(
        BTN,
        INPUT_PULLUP
    );

    pinMode(
        LED,
        OUTPUT
    );

    digitalWrite(
        LED,
        LOW
    );

    pinMode(
        RS485_DE,
        OUTPUT
    );

    digitalWrite(
        RS485_DE,
        LOW
    );

    // ==================================================
    // MQTT CONFIGURATION
    // ==================================================

    loadMQTTConfig();

    // ==================================================
    // WIFI
    // ==================================================

    WiFi.mode(
        WIFI_STA
    );

    connectWiFi();

    // ==================================================
    // MQTT
    // ==================================================

    mqtt.setServer(
        mqttServer.c_str(),
        mqttPort
    );

    mqtt.setCallback(
        mqttCallback
    );

    mqtt.setBufferSize(
        4096
    );

    if (
        WiFi.status() ==
        WL_CONNECTED)
    {
        connectMQTT();
    }

    // ==================================================
    // RS485
    // ==================================================

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

    Serial.println(
        "=============================="
    );

    Serial.println(
        " TUNAS POWER MONITOR"
    );

    Serial.println(
        "=============================="
    );

    Serial.println(
        "OTA GitHub aktif"
    );
}

// ======================================================
// LOOP
// ======================================================

void loop()
{
    // ==================================================
    // BUTTON
    // ==================================================

    handleButton();

    // ==================================================
    // OTA
    // ==================================================

    if (
        otaRequested &&
        !otaRunning)
    {
        otaRequested =
            false;

        otaRunning =
            true;

        // Hentikan request Modbus.
        requestRunning =
            false;

        if (
            WiFi.status() ==
            WL_CONNECTED)
        {
            performOTA();
        }
        else
        {
            Serial.println(
                "OTA dibatalkan: "
                "WiFi tidak terhubung."
            );

            publishOTAStatus(
                "failed_no_wifi"
            );
        }

        otaRunning =
            false;

        return;
    }

    // ==================================================
    // WIFI MANAGER
    // ==================================================

    if (wifiManagerMode)
    {
        handleWiFiManager();

        // Jangan jalankan MQTT/Modbus
        // selama konfigurasi WiFi.
        return;
    }

    // ==================================================
    // WIFI
    // ==================================================

    if (
        WiFi.status() !=
        WL_CONNECTED)
    {
        // Jangan blocking terlalu lama.
        static unsigned long
            lastWiFiReconnect = 0;

        if (
            millis() -
                lastWiFiReconnect >=
            10000UL)
        {
            lastWiFiReconnect =
                millis();

            connectWiFi();
        }
    }

    // ==================================================
    // LED STATUS
    // ==================================================

    handleNormalLED();

    // ==================================================
    // MQTT
    // ==================================================

    if (
        WiFi.status() ==
        WL_CONNECTED)
    {
        if (!mqtt.connected())
        {
            connectMQTT();
        }

        mqtt.loop();
    }

    // ==================================================
    // MODBUS
    // ==================================================

    handleModbus();
}
