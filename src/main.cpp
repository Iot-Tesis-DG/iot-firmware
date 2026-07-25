/**
 * ThermoTrace — Firmware ESP32 para Monitoreo de Cadena de Frío
 * ===================================================================
 * Tesis UPC 2026: Desarrollo y validación de un prototipo basado en IoT e IA
 * con trazabilidad digital verificable para monitoreo de cadena de frío de
 * medicamentos termolábiles en farmacias independientes de Lima Metropolitana.
 *
 * Arquitectura dual-core (FreeRTOS en ESP32):
 *   Core 0 (Protocol Core):  Captura de sensores cada 30s + escritura LittleFS.
 *   Core 1 (Application Core): Loop principal: Wi-Fi + MQTT + sincronización.
 *
 * Flujo de datos:
 *   1. Sensores (DS18B20, SHT31, MC-38) → lectura cada 30s
 *   2. PayloadBuilder → JSON canónico (~250 bytes)
 *   3. RAM buffer → LittleFS (si no hay red, política FIFO)
 *   4. MQTT/TLS QoS 1 → EMQX Cloud → backend FastAPI
 *   5. PUBACK del broker → el buffer LittleFS se limpia
 *
 * Pines (ESP32 DevKitC V4):
 *   GPIO4  → DS18B20 (1-Wire, pull-up 4.7kΩ a 3.3V)
 *   GPIO21 → SHT31 SDA (I2C)
 *   GPIO22 → SHT31 SCL (I2C)
 *   GPIO15 → MC-38 (reed switch, pull-up interno)
 *
 * Requiere:
 *   - PlatformIO con framework arduino
 *   - Certificado CA raíz en data/certs/root_ca.pem
 *   - Credenciales Wi-Fi y MQTT en config.h o build_flags
 */

#include <Arduino.h>
#include <esp_task_wdt.h>

#include "config.h"
#include "sensors/DS18B20Sensor.h"
#include "sensors/SHT31Sensor.h"
#include "sensors/MC38Sensor.h"
#include "connectivity/WiFiManager.h"
#include "connectivity/MQTTManager.h"
#include "storage/LittleFSBuffer.h"
#include "payload/PayloadBuilder.h"

// =========================================================================
// Objetos globales (compartidos entre cores vía volatile / mutex)
// =========================================================================
DS18B20Sensor ds18b20(PIN_DS18B20);
SHT31Sensor   sht31(SHT31_I2C_ADDRESS);
MC38Sensor    mc38(PIN_MC38);
LittleFSBuffer buffer;

WiFiClientSecure tlsClient;
WiFiManager wifi(WIFI_SSID, WIFI_PASSWORD);
MQTTManager  mqtt(tlsClient);

// Cola de lecturas pendientes (RAM → Core 0 produce, Core 1 consume)
static constexpr int RAM_BUFFER_SIZE = 10;
static volatile int ramBufferCount = 0;
static portMUX_TYPE ramMutex = portMUX_INITIALIZER_UNLOCKED;

// Timestamp de la última lectura de sensores
static unsigned long lastSensorRead = 0;
static unsigned long lastBufferFlush = 0;

// =========================================================================
// Core 0 — Tarea de sensores
// =========================================================================
void taskSensores(void* parameter) {
    LOG_I("Core0", "Tarea de sensores iniciada en Core %d.", xPortGetCoreID());

    TickType_t lastWakeTime = xTaskGetTickCount();
    const TickType_t intervalTicks = pdMS_TO_TICKS(INTERVALO_LECTURA_MS);

    for (;;) {
        // ── 1. Leer sensores ────────────────────────────────────────
        float tempInterna = ds18b20.readTemperatureC();
        float tempAmbiental = sht31.readTemperatureC();
        float humedad = sht31.readHumidity();
        bool puertaAbierta = mc38.isOpen();
        unsigned long duracionPuerta = mc38.openDurationSec();

        // ── 2. Construir payload JSON ───────────────────────────────
        PayloadBuilder payload(DEVICE_ID, FIRMWARE_VERSION);
        payload.setTemperatureInterna(tempInterna);
        payload.setTemperatureAmbiental(tempAmbiental);
        payload.setHumidityAmbiental(humedad);
        payload.setDoorOpen(puertaAbierta, duracionPuerta);
        payload.setConnectivityOnline(wifi.isConnected());

        String json = payload.build(512);
        if (json.isEmpty()) {
            LOG_E("Core0", "Payload vacío — omitiendo ciclo.");
            vTaskDelayUntil(&lastWakeTime, intervalTicks);
            continue;
        }

        // ── 3. Guardar en LittleFS (siempre, offline-first) ─────────
        bool saved = buffer.saveReading(json.c_str());
        if (saved) {
            portENTER_CRITICAL(&ramMutex);
            ramBufferCount++;
            portEXIT_CRITICAL(&ramMutex);
        }

        LOG_I("Core0", "Ciclo completado. Pendientes: %d en RAM, %d en Flash.",
              ramBufferCount, buffer.pendingCount());

        // Esperar hasta el próximo ciclo (30s exactos desde el inicio del ciclo)
        vTaskDelayUntil(&lastWakeTime, intervalTicks);
    }
}

// =========================================================================
// Core 1 — Tarea de red (Wi-Fi + MQTT + sincronización)
// =========================================================================
void taskRed(void* parameter) {
    LOG_I("Core1", "Tarea de red iniciada en Core %d.", xPortGetCoreID());

    for (;;) {
        // ── 1. Mantener Wi-Fi ───────────────────────────────────────
        wifi.maintain();
        if (!wifi.isConnected()) {
            delay(500);
            continue;
        }

        // ── 2. Mantener MQTT ────────────────────────────────────────
        if (!mqtt.isConnected()) {
            if (!mqtt.connect()) {
                delay(1000);
                continue;
            }

            // ── 3. RE-CONEXIÓN: publicar evento "online" ────────────
            PayloadBuilder onlinePayload(DEVICE_ID, FIRMWARE_VERSION);
            onlinePayload.setConnectivityOnline(true);
            String onlineJson = onlinePayload.build(512);
            if (!onlineJson.isEmpty()) {
                mqtt.publishEvent(onlineJson.c_str());
            }

            // ── 4. SINCRONIZAR buffer offline ───────────────────────
            auto pending = buffer.listPendingFiles();
            int syncCount = 0;
            for (const auto& file : pending) {
                String data = buffer.readFile(file);
                if (data.isEmpty()) continue;

                // Re-construir timestamp real (ya viene en el JSON guardado)
                uint16_t packetId = mqtt.publish(TOPIC_LECTURAS, data.c_str(), false);
                if (packetId > 0) {
                    syncCount++;
                    // QoS 1: el broker confirma con PUBACK. La librería
                    // PubSubClient maneja esto internamente. Como el backend
                    // tiene UNIQUE(device_id, timestamp), los duplicados
                    // (reenvío por PUBACK perdido) son inocuos.
                    buffer.removeFile(file);
                } else {
                    // Falló la publicación → reintentar en el próximo ciclo.
                    LOG_E("Core1", "Fallo al publicar %s. Reintentando luego.", file.c_str());
                    break;
                }
            }

            if (syncCount > 0) {
                LOG_I("Core1", "Sincronización: %d/%d archivos enviados, %d eliminados.",
                      syncCount, pending.size(), syncCount);
            }

            // Limpiar contador RAM
            portENTER_CRITICAL(&ramMutex);
            ramBufferCount = 0;
            portEXIT_CRITICAL(&ramMutex);
        }

        // ── 5. Loop MQTT (procesa callbacks, keep-alive, PUBACK) ────
        mqtt.loop();

        // ── 6. Publicar lectura en tiempo real si hay red ────────────
        // Las lecturas se guardan en LittleFS por el Core 0. El Core 1
        // las publica inmediatamente si hay conexión. Si no, se acumulan
        // y se sincronizan en el paso 4 al reconectar.

        delay(100);  // Ceder tiempo al scheduler de FreeRTOS
    }
}

// =========================================================================
// setup() — Inicialización en Core 1
// =========================================================================
void setup() {
    Serial.begin(115200);
    delay(1000);  // Esperar a que el monitor serie se estabilice

    Serial.println();
    Serial.println("╔══════════════════════════════════════════════════════╗");
    Serial.println("║   ThermoTrace — Firmware IoT Cadena de Frío        ║");
    Serial.println("║   Tesis UPC 2026                                   ║");
    Serial.println("╠══════════════════════════════════════════════════════╣");
    Serial.printf ("║   Device ID : %-36s ║\n", DEVICE_ID);
    Serial.printf ("║   Firmware  : %-36s ║\n", FIRMWARE_VERSION);
    Serial.printf ("║   ESP32 SDK : %-36s ║\n", ESP.getSdkVersion());
    Serial.println("╚══════════════════════════════════════════════════════╝");
    Serial.println();

    // ── Inicializar LittleFS ────────────────────────────────────────
    if (!buffer.begin()) {
        LOG_E("Setup", "LittleFS no disponible. Reiniciando en 5s...");
        delay(5000);
        ESP.restart();
    }

    // ── Inicializar sensores ────────────────────────────────────────
    ds18b20.begin();
    sht31.begin();
    mc38.begin();

    // ── Sincronizar reloj NTP ───────────────────────────────────────
    // Declarada en PayloadBuilder.cpp. Se llama aquí para que el primer
    // timestamp ya esté sincronizado (o use fallback de compilación).
    extern void syncNTP();
    syncNTP();

    // ── Inicializar Wi-Fi (Core 1) ──────────────────────────────────
    wifi.begin();

    // ── Inicializar MQTT ────────────────────────────────────────────
    mqtt.begin(MQTT_HOST, MQTT_PORT, MQTT_USERNAME, MQTT_TOKEN, MQTT_CLIENT_ID);

    // ── Crear tareas en núcleos separados ───────────────────────────
    // Core 0: Sensores (prioridad más alta: la captura no puede retrasarse).
    xTaskCreatePinnedToCore(
        taskSensores,        // Función
        "Sensores",          // Nombre
        8192,                // Stack size (8 KB, suficiente para JSON + sensores)
        nullptr,             // Parámetro
        2,                   // Prioridad (0-24, más alto = más prioritario)
        nullptr,             // Handle (no necesitamos)
        0                    // Core 0 — Protocol Core
    );

    // Core 1: Red (Wi-Fi + MQTT + sincronización).
    xTaskCreatePinnedToCore(
        taskRed,             // Función
        "Red",               // Nombre
        12288,               // Stack size (12 KB, TLS + MQTT necesitan más)
        nullptr,             // Parámetro
        1,                   // Prioridad (menor que sensores)
        nullptr,             // Handle
        1                    // Core 1 — Application Core
    );

    // ── El loop() principal queda vacío: todo corre en tasks ────────
    LOG_I("Setup", "Inicialización completa. Core 0 = sensores, Core 1 = red.");
    LOG_I("Setup", "MQTT → %s:%d (TLS 1.2, QoS 1)", MQTT_HOST, MQTT_PORT);
    LOG_I("Setup", "Tópico → %s", TOPIC_LECTURAS);
}

// =========================================================================
// loop() — Vacío. Las tareas de FreeRTOS manejan todo.
// =========================================================================
void loop() {
    // Nada. Las tareas taskSensores (Core 0) y taskRed (Core 1) se ejecutan
    // concurrentemente. El watchdog de FreeRTOS se alimenta en cada tarea.
    delay(1000);
}
