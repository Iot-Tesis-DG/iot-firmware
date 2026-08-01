#ifndef MQTT_MANAGER_H
#define MQTT_MANAGER_H

#include <Arduino.h>
#include <MQTT.h>
#include <WiFiClientSecure.h>

#include "../core/ColaFIFO.h"

/**
 * Conexión MQTT sobre TLS 1.2 hacia EMQX Cloud Serverless (puerto 8883).
 *
 * - One-way TLS: valida el certificado del servidor contra la CA raíz.
 * - Autenticación: device_id como usuario + token como contraseña.
 * - SNI: `MQTTClient` conecta por hostname, así que `WiFiClientSecure` envía el
 *   SNI en el handshake igual que antes.
 * - LWT registrado en el CONNECT con QoS 1.
 * - **Publicación en QoS 1 con PUBACK real** (HU-11).
 *
 * POR QUÉ SE CAMBIÓ DE CLIENTE MQTT
 * ---------------------------------
 * `PubSubClient` no implementa QoS 1 al publicar: `publish()` devuelve "escrito
 * en el socket TCP", no "el broker lo recibió". No hay PUBACK, ni packetId, ni
 * forma de confirmarlo. Con eso, la entrega nodo→broker era *at-most-once* y
 * HU-11 no se cumplía, aunque la documentación de la tesis la daba por
 * implementada. La mitigación anterior —revalidar la sesión antes de borrar el
 * archivo— acotaba la ventana pero no la cerraba: un paquete en vuelo cuando
 * caía la sesión se perdía y el archivo ya estaba borrado.
 *
 * `256dpi/MQTT` (arduino-mqtt, sobre lwmqtt) sí lo implementa:
 * `lwmqtt_publish()` con QoS 1 espera el PUBACK dentro del *command timeout* y
 * verifica que el packetId coincida con el enviado; `MQTTClient::publish()`
 * devuelve `true` solo en ese caso. Se eligió sobre `AsyncMqttClient` porque
 * conserva la forma síncrona del código existente (begin/connect/loop/publish)
 * y porque acepta cualquier `Client&`, de modo que **la capa TLS no se toca**:
 * el mismo `WiFiClientSecure` con el mismo `setCACert()`.
 *
 * Contrapartida asumida y acotada: `publish()` ahora BLOQUEA hasta el PUBACK o
 * hasta `MQTT_COMMAND_TIMEOUT_MS`. Eso entra en el presupuesto del watchdog
 * (ver `system/Watchdog.h`) y obliga a no mantener el mutex del buffer durante
 * la publicación (ver `LittleFSBuffer.h`).
 *
 * FALLO CERRADO: si el certificado CA no se puede cargar de LittleFS, o si el
 * nodo no está aprovisionado, `connect()` devuelve false sin intentar nada. No
 * existe ninguna ruta que llame a `setInsecure()`.
 */
class MQTTManager {
public:
    explicit MQTTManager(WiFiClientSecure& tlsClient);

    void begin(const char* host, uint16_t port,
               const char* username, const char* password,
               const char* clientId);

    bool connect();
    bool isConnected();
    bool loop();  // Debe llamarse frecuentemente para keep-alive y callbacks

    /// Publica una lectura en QoS 1 y espera el PUBACK del broker.
    /// `Confirmado` solo se devuelve con el PUBACK recibido: es la única
    /// condición bajo la que el llamador puede borrar su única copia.
    core::ResultadoPublicacion publicarLectura(const char* topic, const char* payload);

    /// Publica un evento de conectividad (QoS 1, sin retención).
    core::ResultadoPublicacion publicarEvento(const char* eventJson);

    WiFiClientSecure& getClient() { return _client; }

private:
    WiFiClientSecure& _client;

    // Buffers de lectura y escritura de lwmqtt. 1024 B cubre de sobra el
    // payload de lectura (≤512 B) y el LWT (~110 B), y deja margen para los
    // paquetes de control del broker.
    MQTTClient _mqtt{1024, 1024};

    const char* _host = nullptr;
    uint16_t _port = 8883;
    const char* _username = nullptr;
    const char* _password = nullptr;
    const char* _clientId = nullptr;

    // Copias propias de los valores cuyo ciclo de vida no controlamos.
    // `MQTTClient` y `WiFiClientSecure` guardan punteros, no contenido.
    String _hostStr;
    String _passwordStr;
    String _caPem;

    bool _caCargado = false;
    bool _credencialesOk = false;

    unsigned long _lastConnectAttempt = 0;
    static constexpr unsigned long RECONNECT_COOLDOWN_MS = 5000;

    bool _loadCACertificate(const char* path);
    static void _mqttCallback(String& topic, String& payload);
};

#endif  // MQTT_MANAGER_H
