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
 *   4. MQTT/TLS (QoS 0 al publicar; el LWT sí va en QoS 1) → EMQX → backend
 *   5. Publicado y sesión aún viva → se libera el archivo de LittleFS
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

// Contador informativo de lecturas capturadas desde la última publicación.
// Solo alimenta el log del Core 0; la cola real vive en LittleFS.
static volatile int ramBufferCount = 0;
static portMUX_TYPE ramMutex = portMUX_INITIALIZER_UNLOCKED;

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

        // La puerta se muestrea cada 50 ms durante toda la ventana (ver la
        // espera al final del ciclo), no aquí. Se reporta si hubo apertura en
        // algún momento de los 30 s, no si justo estaba abierta al muestrear:
        // una apertura de 15 s entre dos muestras se perdía por completo.
        bool puertaAbierta = mc38.huboApertura();
        unsigned long duracionPuerta = mc38.duracionAperturaSegundos();

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

        // Ventana de reporte cerrada: lo que venga a partir de aquí cuenta
        // para el siguiente payload.
        mc38.limpiarReporte();

        LOG_I("Core0", "Ciclo completado. Pendientes: %d en RAM, %d en Flash.",
              ramBufferCount, buffer.pendingCount());

        // ── 4. Esperar al próximo ciclo MUESTREANDO la puerta ───────
        //
        // La cadencia sigue anclada a `lastWakeTime`, así que el jitter no se
        // acumula y los 30 s del RF siguen siendo 30 s. Pero en vez de dormir
        // de un tirón, se despierta cada 50 ms para llamar a `mc38.poll()`:
        // sin esto el antirrebote no tiene muestras sobre las que actuar y las
        // aperturas que empiezan y terminan dentro de la ventana no existen.
        //
        // Solo la puerta necesita este trato: temperatura y humedad son
        // magnitudes lentas y muestrearlas cada 30 s es correcto.
        const TickType_t pasoPoll = pdMS_TO_TICKS(MC38Sensor::POLL_MS);
        for (;;) {
            // Diferencia con signo: sobrevive al desbordamiento del contador de
            // ticks (~49 días con tick de 1 kHz), donde una resta sin signo
            // daría un valor enorme y colgaría el muestreo.
            const int32_t faltan =
                (int32_t)(lastWakeTime + intervalTicks - xTaskGetTickCount());
            if (faltan <= (int32_t)pasoPoll) break;
            vTaskDelay(pasoPoll);
            mc38.poll();
        }
        vTaskDelayUntil(&lastWakeTime, intervalTicks);
    }
}

// =========================================================================
// Core 1 — Tarea de red (Wi-Fi + MQTT + sincronización)
// =========================================================================
// Archivos publicados como máximo en una pasada del bucle de red.
//
// Con la cola llena (200 archivos) y un handshake TLS por delante, drenarla
// entera en una sola iteración deja de alimentar al watchdog: el propio manual
// avisa de que una operación bloqueante de más de 5 s reinicia el ESP32
// (§7.2). Se drena a ritmo acotado, cediendo CPU entre lotes; con lecturas
// cada 30 s, 20 por pasada vacían el backlog completo en pocos segundos.
static constexpr int MAX_PUBLICACIONES_POR_CICLO = 20;

/// Publica lecturas pendientes de LittleFS en orden FIFO.
/// Devuelve cuántas se entregaron. Se detiene al primer fallo: el orden
/// cronológico de la cadena de evidencia importa más que el rendimiento.
static int drenarBuffer() {
    auto pending = buffer.listPendingFiles();
    if (pending.empty()) return 0;

    int enviados = 0;
    for (const auto& file : pending) {
        if (enviados >= MAX_PUBLICACIONES_POR_CICLO) break;

        String data = buffer.readFile(file);
        if (data.isEmpty()) {
            // Archivo ilegible (corte de corriente a mitad de escritura): no
            // puede reintentarse eternamente ni debe frenar la cola.
            LOG_E("Core1", "Archivo ilegible %s — descartado.", file.c_str());
            buffer.removeFile(file);
            continue;
        }

        if (mqtt.publish(TOPIC_LECTURAS, data.c_str(), false) == 0) {
            LOG_E("Core1", "Fallo al publicar %s. Reintentando luego.", file.c_str());
            break;
        }

        // `publish()` de PubSubClient es QoS 0: confirma que el paquete salió
        // por TCP, no que el broker lo recibiera. Antes de borrar la única
        // copia se comprueba que la sesión siga viva; si se cayó a mitad del
        // envío, el archivo se conserva y se reintenta. No es equivalente a un
        // PUBACK — ver la nota de garantías de entrega en MQTTManager::publish.
        mqtt.loop();
        if (!mqtt.isConnected()) {
            LOG_E("Core1", "Sesión caída tras publicar %s — se conserva.", file.c_str());
            break;
        }

        buffer.removeFile(file);
        enviados++;
    }
    return enviados;
}

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

            // ── 3. RE-CONEXIÓN ──────────────────────────────────────
            //
            // Aquí NO se publica ningún evento "online": ya lo hace
            // `MQTTManager::connect()` con `LWT_PAYLOAD_ONLINE`, que tiene la
            // forma que el backend espera en `/eventos` (`tipo_evento`).
            //
            // Antes se construía un `PayloadBuilder` —es decir, un JSON de
            // LECTURA— y se publicaba en el tópico de EVENTOS. El backend lo
            // valida con `EventoDispositivoPayload` (extra="forbid", exige
            // `tipo_evento`), así que fallaba siempre: cada reconexión
            // generaba un ValidationError descartado con un warning.
            //
            // Con la Wi-Fi ya levantada, este es además el momento de
            // recuperar el reloj si NTP no pudo sincronizar en el arranque.
            extern bool ntpEstaSincronizado();
            extern void syncNTP();
            if (!ntpEstaSincronizado()) {
                LOG_I("Core1", "Reintentando sincronización NTP...");
                syncNTP();
            }
        }

        // ── 4. Procesar keep-alive y callbacks ──────────────────────
        mqtt.loop();

        // ── 5. Publicar la cola pendiente ───────────────────────────
        //
        // Se ejecuta en CADA pasada con conexión, no solo al reconectar.
        // Antes vivía dentro del `if (!mqtt.isConnected())` de arriba, así que
        // en régimen normal —conectado— no se publicaba absolutamente nada:
        // el Core 0 seguía escribiendo en LittleFS cada 30 s, la cola crecía
        // hasta 200 y el FIFO empezaba a tirar las lecturas más antiguas. El
        // dashboard solo veía datos justo después de una reconexión.
        //
        // Camino único para telemetría en vivo y para el backlog offline: el
        // Core 0 siempre persiste antes (offline-first, sobrevive a un corte de
        // corriente) y el Core 1 publica en orden FIFO. Con el bucle a 100 ms,
        // una lectura recién guardada sale muy por debajo del techo de 5 s del
        // RNF-01.
        int enviados = drenarBuffer();
        if (enviados > 0) {
            LOG_I("Core1", "Publicadas %d lecturas. Quedan %d en Flash.",
                  enviados, buffer.pendingCount());
            portENTER_CRITICAL(&ramMutex);
            ramBufferCount = 0;
            portEXIT_CRITICAL(&ramMutex);
        }

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
    //
    // Un sensor ausente NO detiene el arranque: el nodo sigue publicando con
    // ese campo en null y el resto de la cadena funciona. Pero debe quedar
    // dicho en el monitor serie, porque el síntoma aguas abajo —un null en el
    // dashboard— no distingue "sensor mal cableado" de "lectura no disponible".
    // Ambos drivers reintentan la detección en cada ciclo.
    ds18b20.begin();
    if (!sht31.begin()) {
        LOG_E("Setup", "SHT31 no detectado en 0x%02X: revisar SDA=GPIO%d / SCL=GPIO%d.",
              SHT31_I2C_ADDRESS, 21, 22);
    }
    mc38.begin();

    // ── Inicializar Wi-Fi (Core 1) ──────────────────────────────────
    wifi.begin();

    // ── Sincronizar reloj NTP ───────────────────────────────────────
    // DESPUÉS de levantar la Wi-Fi, no antes: NTP viaja por UDP/123 y sin red
    // `getLocalTime()` se limita a agotar sus 10 s de espera y fallar. Con el
    // orden anterior la sincronización NO podía funcionar en ningún arranque,
    // así que todos los timestamps salían de la hora de COMPILACIÓN más
    // `millis()`. Pasadas 48 h desde el flasheo, el backend empieza a
    // rechazarlos por `timestamp_demasiado_antiguo` (ANTIGUEDAD_MAXIMA) y el
    // nodo deja de registrar sin ningún error visible en el monitor serie.
    extern void syncNTP();
    if (wifi.isConnected()) {
        syncNTP();
    } else {
        // Sin red al arrancar se usa el fallback de compilación; el reloj se
        // corrige en el primer ciclo de red con conexión.
        LOG_E("Setup", "Sin Wi-Fi al arrancar: NTP se reintentará al conectar.");
    }

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
    LOG_I("Setup", "MQTT → %s:%d (TLS 1.2, publicacion QoS 0, LWT QoS 1)", MQTT_HOST, MQTT_PORT);
    LOG_I("Setup", "Tópico → %s", TOPIC_LECTURAS);
}

// =========================================================================
// loop() — Vacío. Las tareas de FreeRTOS manejan todo.
// =========================================================================
void loop() {
    // Nada. taskSensores (Core 0) y taskRed (Core 1) corren concurrentemente.
    //
    // WATCHDOG: ninguna de las dos tareas está suscrita al Task WDT.
    // El comentario anterior afirmaba que "el watchdog se alimenta en cada
    // tarea", pero `esp_task_wdt_add()` no se llama en ninguna parte: sólo
    // `loopTask` queda cubierta por el TWDT que inicializa Arduino. Si
    // `taskRed` se quedara colgada —por ejemplo dentro del handshake TLS— nada
    // reiniciaría el nodo.
    //
    // No se suscriben aquí a propósito: el TWDT de Arduino viene con 5 s de
    // plazo y en esta tarea hay dos bloqueos legítimos más largos (15 s de
    // conexión Wi-Fi, 10 s de NTP). Suscribirlas sin subir antes el plazo y sin
    // sembrar `esp_task_wdt_reset()` dentro de esos bucles provocaría un ciclo
    // de reinicios, que es peor que no tener watchdog. Queda como mejora
    // pendiente, a validar sobre hardware real.
    delay(1000);
}
