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
// HU-07: acuse LÓGICO de aplicación — el backend publica aquí solo después
// de que la lectura hizo COMMIT en PostgreSQL. Distinto del PUBACK de
// transporte QoS1 (eso solo confirma que el broker recibió el mensaje, no
// que el backend lo persistió). Ver interface/main.py::_topico_ack().
#define TOPIC_ACK               "farmacias/" DEVICE_ID "/ack"

// HU-07: cuánto espera el nodo el acuse lógico antes de considerar la
// publicación fallida y reintentar en el próximo ciclo. Debe cubrir con
// holgura una escritura Postgres real (decenas de ms) más el RTT a EMQX
// Cloud; 8 s es 60% más que MQTT_COMMAND_TIMEOUT_MS porque el acuse lógico
// llega DESPUÉS del PUBACK, no en paralelo con él.
#define ACK_LOGICO_TIMEOUT_MS   8000

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

// HU-04: no todos los nodos tienen el MC-38 instalado. Con el pull-up
// interno, un pin sin sensor conectado flota en HIGH — el mismo nivel que
// "puerta abierta" — así que sin este flag un nodo sin MC-38 reportaría una
// apertura constante en vez de "no aplica". Se define en build_flags por
// nodo (-DMC38_INSTALADO=0); por defecto 1 porque es la configuración de
// referencia del prototipo.
#ifndef MC38_INSTALADO
  #define MC38_INSTALADO 1
#endif

// =========================================================================
// HU-52: continuidad de medición ante corte de suministro eléctrico
// =========================================================================
// Hardware no validado en el prototipo actual: requiere el circuito de
// respaldo de 5V (exclusivo para ESP32+sensores, nunca el refrigerador ni el
// router) y su sensado. RESPALDO_INSTALADO=0 por defecto, mismo patrón que
// MC38_INSTALADO — un nodo sin el circuito no intenta leer pines que no
// existen ni publica eventos de un subsistema que no tiene.
#ifndef RESPALDO_INSTALADO
  #define RESPALDO_INSTALADO 0
#endif
// GPIO digital hacia el comparador del circuito de respaldo: HIGH mientras
// hay suministro comercial, LOW al conmutar a la batería/respaldo de 5V.
#define PIN_RESPALDO_SENSE          27
// Entrada ADC sobre el divisor de tensión del respaldo, para estimar su
// nivel restante. Rango y umbral se calibran en el piloto contra el circuito
// real (HU-52 escenario 3: "la autonomía real del respaldo se mide y
// documenta... no se fija por supuesto") — este valor es un punto de partida
// conservador, no una autonomía comprometida.
#define PIN_RESPALDO_BATERIA_ADC    34
#define UMBRAL_RESPALDO_BATERIA_ADC 1200
// Cada cuánto se relee el estado del respaldo desde taskRed — no hace falta
// la cadencia de 30s de los sensores térmicos, esto es diagnóstico de
// infraestructura, no la variable del experimento.
#define INTERVALO_CHEQUEO_RESPALDO_MS 10000

// Intervalos
#define INTERVALO_LECTURA_MS   30000  // 30 segundos — cadencia de muestreo

// HU-44 escenario 2: cada cuánto se relee el token MQTT de NVS para detectar
// una rotación/revocación aplicada por un técnico sin reiniciar el nodo. 5
// minutos es un compromiso entre "el corte de acceso surte efecto pronto" y
// "no abrir `Preferences` (NVS) en cada vuelta del bucle de 100 ms".
#define MQTT_TOKEN_POLL_INTERVAL_MS 300000

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

// HU-08 criterio 3: a partir de este número de intentos fallidos
// consecutivos (sin reconectar), se considera que la ausencia de red supera
// lo operativamente normal (AP fuera de alcance, credenciales inválidas) y
// se activa la señal de diagnóstico — 5 intentos ya acumulan al menos
// 1+2+4+8+16 = 31 s de backoff, más allá de una caída momentánea del AP.
#define WIFI_UMBRAL_DIAGNOSTICO_INTENTOS 5

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
