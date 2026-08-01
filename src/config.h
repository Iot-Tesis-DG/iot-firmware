#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>
#include <WiFiClientSecure.h>

// =========================================================================
// Identidad del dispositivo — única por nodo. CAMBIAR para cada ESP32 físico.
// =========================================================================
#ifndef DEVICE_ID
  #define DEVICE_ID            "FARM-01-CDL"
#endif
#define FIRMWARE_VERSION       "1.0.0"

// =========================================================================
// CREDENCIALES — RNF-05: cero credenciales embebidas en el código
// =========================================================================
// Las macros de abajo son cadenas VACÍAS a propósito. Antes tenían valores por
// defecto en texto plano ("cambiar_en_produccion", "token_generado_en_emqx")
// que se compilaban dentro del binario. Aunque fueran marcadores, el efecto
// práctico era doble y malo: (a) el firmware compilaba y arrancaba con
// credenciales inválidas fallando en el broker con un error genérico, en vez de
// decir que no estaba aprovisionado; y (b) el patrón invitaba a editar este
// archivo con las credenciales reales y subirlo al repositorio, que es
// exactamente lo que RNF-05 prohíbe.
//
// Orden de precedencia en tiempo de ejecución (ver `system/Credenciales.h`):
//   1. NVS  — partición cifrable, se aprovisiona por dispositivo sin recompilar
//   2. build_flags de PlatformIO (-DWIFI_PASSWORD='"..."'), para CI y taller
//   3. nada → el nodo NO intenta conectar y lo dice por el monitor serie
//
// El nodo sigue capturando y almacenando en LittleFS sin credenciales: la
// cadena de frío se registra aunque el aprovisionamiento esté pendiente.
#ifndef WIFI_SSID
  #define WIFI_SSID            ""
#endif
#ifndef WIFI_PASSWORD
  #define WIFI_PASSWORD        ""
#endif

// =========================================================================
// MQTT / EMQX Cloud Serverless — TLS 1.2 puerto 8883
// =========================================================================
#ifndef MQTT_HOST
  #define MQTT_HOST            ""
#endif
#define MQTT_PORT              8883
#ifndef MQTT_TOKEN
  #define MQTT_TOKEN           ""
#endif
#define MQTT_USERNAME          DEVICE_ID              // device_id como usuario
#define MQTT_CLIENT_ID         DEVICE_ID

/// Espacio de nombres NVS del que se leen las credenciales aprovisionadas.
#define CREDENCIALES_NVS_NS    "thermotrace"

// Topics
#define TOPIC_LECTURAS         "farmacias/" DEVICE_ID "/lecturas"
#define TOPIC_EVENTOS          "farmacias/" DEVICE_ID "/eventos"

// LWT — el broker publica esto si el ESP32 se cae
#define TOPIC_LWT              "farmacias/" DEVICE_ID "/eventos"
#define LWT_PAYLOAD_OFFLINE    "{\"device_id\":\"" DEVICE_ID "\",\"tipo_evento\":\"lwt_offline\",\"timestamp\":\"1970-01-01T00:00:00Z\"}"
#define LWT_PAYLOAD_ONLINE     "{\"device_id\":\"" DEVICE_ID "\",\"tipo_evento\":\"lwt_online\",\"timestamp\":\"1970-01-01T00:00:00Z\"}"

// QoS 1 tanto en el LWT (willQos del CONNECT) como en la PUBLICACIÓN de
// lecturas. Con PubSubClient esto era imposible —solo publicaba en QoS 0— y
// HU-11 no se cumplía; ver la nota de migración en MQTTManager.h.
#define MQTT_QOS               1
#define MQTT_RETAIN            0

// Keep-alive MQTT: si el broker no recibe PING en este tiempo, dispara LWT.
#define MQTT_KEEPALIVE_SEC     60

// Command timeout de lwmqtt: cuánto se espera un CONNACK o un PUBACK antes de
// darlo por perdido. Es el tiempo que `publish()` puede llegar a BLOQUEAR, así
// que entra directamente en el presupuesto del watchdog (system/Watchdog.h).
// 5 s cubre con holgura el RTT a EMQX Cloud sobre TLS en una red de farmacia.
#define MQTT_COMMAND_TIMEOUT_MS 5000

// =========================================================================
// Sensores — pines GPIO del ESP32 DevKitC V4
// =========================================================================
#define PIN_DS18B20            4     // 1-Wire (GPIO4, pull-up 4.7kΩ a 3.3V)
#define PIN_MC38               15    // Reed switch (GPIO15, pull-up interno)
#define SHT31_I2C_ADDRESS      0x44  // Dirección I2C por defecto del SHT31-DIS

// Intervalos
#define INTERVALO_LECTURA_MS   30000  // 30 segundos — cadencia de muestreo

// =========================================================================
// Buffer offline LittleFS
// =========================================================================
#define LITTLEFS_MAX_FILES     200    // ~200 lecturas ≈ 100 minutos offline
#define LITTLEFS_MAX_FILESIZE  512    // Bytes máximos por archivo de lectura
#define LITTLEFS_MOUNT_POINT   "/littlefs"

// =========================================================================
// Backoff exponencial para reconexión Wi-Fi
// =========================================================================
#define WIFI_RECONNECT_BASE_MS   1000    // 1s inicial
#define WIFI_RECONNECT_MAX_MS    60000   // 60s tope
#define WIFI_RECONNECT_FACTOR    2       // Duplicar cada intento
#define WIFI_CONNECT_TIMEOUT_MS  15000   // Espera máxima por asociación

// =========================================================================
// Timeout de operaciones
// =========================================================================
// 2000 ms, no 1000. La conversión del DS18B20 a 12 bits tarda hasta 750 ms
// según hoja de datos, y el sondeo se hace en pasos de `delay(10)` desde una
// tarea que comparte núcleo: 1000 ms dejaba un margen del 25 % y cualquier
// pico de planificación marcaba el sensor como averiado. Es el sensor que va
// junto al medicamento, así que un falso negativo aquí cuesta la variable
// principal del experimento.
#define SENSOR_READ_TIMEOUT_MS  2000
#define TLS_HANDSHAKE_TIMEOUT_MS 10000

// Espera de `getLocalTime()` al sincronizar NTP. Es un bloqueo NO alimentable
// (una sola llamada al SDK que no devuelve el control), así que forma parte del
// presupuesto del watchdog: ver el `static_assert` de system/Watchdog.h.
#define NTP_SYNC_TIMEOUT_MS     10000

// =========================================================================
// Certificado raíz de la CA para TLS — EMQX Cloud / Let's Encrypt
// Se almacena en LittleFS (data/certs/root_ca.pem) para no embeberlo
// en el binario y poder actualizarlo sin recompilar.
// =========================================================================
#define CERT_FILE              "/certs/root_ca.pem"

// =========================================================================
// Macros de depuración
// =========================================================================
#ifdef DEBUG_IOT
  #define LOG_I(tag, fmt, ...)   Serial.printf("[%s] " fmt "\n", tag, ##__VA_ARGS__)
#else
  #define LOG_I(tag, fmt, ...)   ((void)0)
#endif
#define LOG_E(tag, fmt, ...)     Serial.printf("[%s] ERROR: " fmt "\n", tag, ##__VA_ARGS__)

#endif // CONFIG_H
