#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>
#include <WiFiClientSecure.h>

// =========================================================================
// Identidad del dispositivo — única por nodo. CAMBIAR para cada ESP32 físico.
// =========================================================================
#define DEVICE_ID              "FARM-01-CDL"
#define FIRMWARE_VERSION       "1.0.0"

// =========================================================================
// Wi-Fi — credenciales inyectadas desde variables de entorno de PlatformIO
// o definidas aquí para desarrollo local. En producción usar build_flags.
// =========================================================================
#ifndef WIFI_SSID
  #define WIFI_SSID           "THERMOSAFE_IOT"
#endif
#ifndef WIFI_PASSWORD
  #define WIFI_PASSWORD       "cambiar_en_produccion"
#endif

// =========================================================================
// MQTT / EMQX Cloud Serverless — TLS 1.2 puerto 8883
// =========================================================================
// Con guarda, igual que WIFI_* y MQTT_TOKEN: así el hostname del broker puede
// inyectarse desde `build_flags` (-DMQTT_HOST='"..."') sin editar este archivo
// ni arriesgarse a subirlo al repositorio.
#ifndef MQTT_HOST
  #define MQTT_HOST            "tu-instancia.emqx.cloud"
#endif
#define MQTT_PORT              8883
#define MQTT_USERNAME          DEVICE_ID              // device_id como usuario
#ifndef MQTT_TOKEN
  #define MQTT_TOKEN           "token_generado_en_emqx"
#endif
#define MQTT_CLIENT_ID         DEVICE_ID

// Topics
#define TOPIC_LECTURAS         "farmacias/" DEVICE_ID "/lecturas"
#define TOPIC_EVENTOS          "farmacias/" DEVICE_ID "/eventos"

// LWT — el broker publica esto si el ESP32 se cae
#define TOPIC_LWT              "farmacias/" DEVICE_ID "/eventos"
#define LWT_PAYLOAD_OFFLINE    "{\"device_id\":\"" DEVICE_ID "\",\"tipo_evento\":\"lwt_offline\",\"timestamp\":\"1970-01-01T00:00:00Z\"}"
#define LWT_PAYLOAD_ONLINE     "{\"device_id\":\"" DEVICE_ID "\",\"tipo_evento\":\"lwt_online\",\"timestamp\":\"1970-01-01T00:00:00Z\"}"

// MQTT_QOS aplica al LWT (parametro willQos de CONNECT). La publicacion de
// lecturas va en QoS 0: PubSubClient no soporta QoS 1 al publicar.
#define MQTT_QOS               1
#define MQTT_RETAIN            0

// Keep-alive MQTT: si el broker no recibe PING en este tiempo, dispara LWT.
#define MQTT_KEEPALIVE_SEC     60

// =========================================================================
// Sensores — pines GPIO del ESP32 DevKitC V4
// =========================================================================
#define PIN_DS18B20            4     // 1-Wire (GPIO4, pull-up 4.7kΩ a 3.3V)
#define PIN_MC38               15    // Reed switch (GPIO15, pull-up interno)
#define SHT31_I2C_ADDRESS      0x44  // Dirección I2C por defecto del SHT31-DIS

// Intervalos
#define INTERVALO_LECTURA_MS   30000  // 30 segundos — cadencia de muestreo
#define INTERVALO_GUARDADO_MS  60000  // 60 segundos — volcado RAM→LittleFS

// =========================================================================
// Buffer offline LittleFS
// =========================================================================
#define LITTLEFS_MAX_FILES     200    // Máximo de archivos en buffer (~200 lecturas ≈ 100 minutos offline)
#define LITTLEFS_MAX_FILESIZE  512    // Bytes máximos por archivo de lectura
#define LITTLEFS_MOUNT_POINT   "/littlefs"

// =========================================================================
// Backoff exponencial para reconexión Wi-Fi
// =========================================================================
#define WIFI_RECONNECT_BASE_MS   1000    // 1s inicial
#define WIFI_RECONNECT_MAX_MS    60000   // 60s tope
#define WIFI_RECONNECT_FACTOR    2       // Duplicar cada intento

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
#define MQTT_CONNECT_TIMEOUT_MS 15000
#define MQTT_PUBLISH_TIMEOUT_MS 5000
#define TLS_HANDSHAKE_TIMEOUT_MS 10000

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
