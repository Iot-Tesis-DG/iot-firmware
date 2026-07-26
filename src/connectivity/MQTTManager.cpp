#include "MQTTManager.h"
#include "../config.h"
#include <LittleFS.h>

// =========================================================================
// Singleton para el callback estático de PubSubClient
// =========================================================================
static MQTTManager* _instance = nullptr;

MQTTManager::MQTTManager(WiFiClientSecure& tlsClient)
    : _client(tlsClient), _mqtt(tlsClient) {
    _instance = this;
}

bool MQTTManager::_loadCACertificate(const char* path) {
    File cert = LittleFS.open(path, "r");
    if (!cert) {
        LOG_E("MQTT", "No se pudo abrir el certificado CA: %s", path);
        LOG_E("MQTT", "Ejecutar: 'pio run --target uploadfs' para flashear data/certs/");
        return false;
    }

    String ca = cert.readString();
    cert.close();

    _client.setCACert(ca.c_str());
    LOG_I("MQTT", "Certificado CA cargado (%d bytes).", ca.length());
    return true;
}

void MQTTManager::begin(const char* host, uint16_t port,
                         const char* username, const char* password,
                         const char* clientId) {
    _host = host;
    _port = port;
    _username = username;
    _password = password;
    _clientId = clientId;

    // TLS 1.2.
    //
    // `WiFiClientSecure` del core Arduino-ESP32 NO expone un selector de
    // versión mínima (`setMinSupportedTLS` es API de ESP8266/BearSSL y aquí no
    // existe: su uso impedía compilar). No hace falta: mbedTLS se compila en
    // ESP-IDF con TLS 1.2 como único protocolo habilitado, así que el suelo ya
    // es 1.2 y no hay downgrade posible a 1.0/1.1.
    //
    // La garantía que sí depende de nosotros es la autenticación del servidor:
    // `setCACert()` más abajo. Sin ella el handshake aceptaría cualquier
    // certificado, que es el riesgo real (MITM), no la versión del protocolo.

    // Timeouts
    _client.setTimeout(TLS_HANDSHAKE_TIMEOUT_MS / 1000);
    _client.setHandshakeTimeout(TLS_HANDSHAKE_TIMEOUT_MS / 1000);

    // SNI: el hostname del broker se envía durante el handshake TLS
    // para que el servidor presente el certificado correcto.
    // Libre de hacer en WiFiClientSecure de ESP32 — se configura automáticamente
    // al conectar al host.

    // Cargar certificado CA desde LittleFS
    if (!_loadCACertificate(CERT_FILE)) {
        LOG_E("MQTT", "Sin certificado CA, TLS no podrá validar el servidor.");
    }

    _mqtt.setServer(_host, _port);
    _mqtt.setCallback(_mqttCallback);

    // Keep-alive explícito. `MQTT_KEEPALIVE_SEC` estaba definido en config.h
    // pero nunca se aplicaba, así que regía el valor por defecto de
    // PubSubClient (15 s) y no los 60 s documentados. Importa porque este plazo
    // es el que decide cuánto tarda EMQX en dar el nodo por muerto y publicar
    // su LWT: es la latencia de detección de una caída (§3.6).
    _mqtt.setKeepAlive(MQTT_KEEPALIVE_SEC);

    // Buffer de recepción ampliado para los mensajes de control del broker
    _mqtt.setBufferSize(1024);

    LOG_I("MQTT", "Configurado: %s:%d (TLS 1.2, publicacion QoS 0, LWT QoS 1).", _host, _port);
}

bool MQTTManager::connect() {
    if (_mqtt.connected()) return true;

    unsigned long ahora = millis();
    if (ahora - _lastConnectAttempt < RECONNECT_COOLDOWN_MS) {
        return false;
    }
    _lastConnectAttempt = ahora;

    LOG_I("MQTT", "Conectando a %s:%d como '%s'...", _host, _port, _clientId);

    // Configurar LWT ANTES de conectar: si el ESP32 se apaga sin enviar
    // DISCONNECT, el broker publica este mensaje automáticamente.
    // El timestamp falso será reemplazado por el broker o el backend lo
    // interpretará como "el dispositivo se cayó".
    if (!_mqtt.connect(
            _clientId,
            _username,       // device_id
            _password,       // token
            TOPIC_LWT,       // topic del LWT
            MQTT_QOS,
            MQTT_RETAIN,
            LWT_PAYLOAD_OFFLINE  // "offline" si el nodo se cae
        )) {
        int estado = _mqtt.state();
        LOG_E("MQTT", "Conexión fallida. Estado MQTT: %d", estado);
        switch (estado) {
            case -4: LOG_E("MQTT", "  → Timeout de conexión (¿EMQX accesible?)."); break;
            case -2: LOG_E("MQTT", "  → Error de red (¿Wi-Fi caído?)."); break;
            case -1: LOG_E("MQTT", "  → Timeout de handshake TLS (¿certificado CA correcto?)."); break;
            case  4: LOG_E("MQTT", "  → Credenciales inválidas (MQTT_CREDENTIALS_REJECTED)."); break;
            case  5: LOG_E("MQTT", "  → No autorizado (MQTT_NOT_AUTHORIZED)."); break;
            default: break;
        }
        return false;
    }

    LOG_I("MQTT", "Conectado a EMQX. Publicando LWT 'online'...");

    // Nada más conectar, publicar evento de que estamos vivos.
    // El backend actualiza estado_conectividad = true al recibir esto.
    _mqtt.publish(TOPIC_LWT, LWT_PAYLOAD_ONLINE, false);

    LOG_I("MQTT", "Listo. Publicando en '%s'.", TOPIC_LECTURAS);
    return true;
}

bool MQTTManager::isConnected() {
    return _mqtt.connected();
}

bool MQTTManager::loop() {
    return _mqtt.loop();
}

uint16_t MQTTManager::publish(const char* topic, const char* payload, bool retained) {
    if (!_mqtt.connected()) {
        LOG_E("MQTT", "No conectado. Imposible publicar.");
        return 0;
    }

    // GARANTÍA REAL DE ENTREGA — leer antes de confiar en esto.
    //
    // `PubSubClient::publish()` publica SIEMPRE en QoS 0. La librería no ofrece
    // QoS 1 en publicación: no existe PUBACK, ni packetId, ni callback de
    // confirmación (el `onPublishAck` que este código declaraba nunca llegó a
    // invocarse). El valor devuelto significa "el paquete se escribió en el
    // socket TLS", no "el broker lo recibió".
    //
    // Consecuencia honesta: la entrega es *at-most-once* en el salto nodo →
    // broker. Si la sesión se corta con el paquete en vuelo, esa lectura se
    // pierde. Quien la borra del buffer es el llamador, que antes revalida la
    // sesión (ver `drenarBuffer()` en main.cpp) — mitiga la ventana, no la
    // elimina.
    //
    // Lo que sí sigue en pie:
    //   - El LWT sí va con QoS 1: se negocia en CONNECT (parámetro willQos).
    //   - `UNIQUE(device_id, timestamp)` en el backend hace inocuo cualquier
    //     reenvío duplicado, así que reintentar siempre es seguro.
    //
    // Para *at-least-once* de verdad hay que cambiar de cliente MQTT
    // (AsyncMqttClient expone PUBACK con packetId). Está anotado como mejora
    // en IoT-documentacion_iot.md §8.2.
    if (!_mqtt.publish(topic, (const uint8_t*)payload, strlen(payload), retained)) {
        LOG_E("MQTT", "Fallo en publish().");
        return 0;
    }

    return 1;  // Escrito en el socket (no confirmado por el broker)
}

uint16_t MQTTManager::publishEvent(const char* eventJson) {
    return publish(TOPIC_EVENTOS, eventJson, false);
}

// =========================================================================
// Callback estático — PubSubClient no acepta lambdas con captura
// =========================================================================
void MQTTManager::_mqttCallback(char* topic, byte* payload, unsigned int length) {
    // Solo nos interesa confirmar que el mensaje se publicó.
    // PubSubClient llama a este callback para mensajes entrantes en topics
    // suscritos. El ESP32 no se suscribe a ningún topic, así que esto
    // solo se activaría si el broker envía algo inesperado.
    LOG_I("MQTT", "Mensaje recibido en topic inesperado: %s (ignorado).", topic);
}
