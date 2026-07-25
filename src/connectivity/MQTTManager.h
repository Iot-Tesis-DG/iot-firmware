#ifndef MQTT_MANAGER_H
#define MQTT_MANAGER_H

#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>

/**
 * Conexión MQTT sobre TLS 1.2 hacia EMQX Cloud Serverless (puerto 8883).
 *
 * - One-way TLS: valida el certificado del servidor contra la CA raíz.
 * - Autenticación: device_id como usuario + token como contraseña.
 * - SNI (Server Name Indication): configurado en el cliente TLS.
 * - LWT (Last Will and Testament): el broker publica automaticamente
 *   "offline" si el ESP32 se desconecta abruptamente.
 * - QoS 1: el ESP32 retiene cada lectura en LittleFS hasta recibir PUBACK.
 * - Callback: las confirmaciones de publicación (PUBACK) se reciben aquí.
 */
class MQTTManager {
public:
    using OnPublishAck = void (*)(uint16_t packetId, bool success);

    MQTTManager(WiFiClientSecure& tlsClient);

    void begin(const char* host, uint16_t port,
               const char* username, const char* password,
               const char* clientId);

    bool connect();
    bool isConnected() const;
    bool loop();  // Debe llamarse frecuentemente para procesar callbacks

    /// Publica un payload JSON. Retorna packetId (>0) si se encoló para envío.
    /// QoS 1: el mensaje no se considera entregado hasta que llegue PUBACK.
    uint16_t publish(const char* topic, const char* payload, bool retained = false);

    /// Registra callback que se invoca cuando el broker confirma la publicación
    /// (PUBACK para QoS 1). El firmware usa esto para liberar el buffer LittleFS.
    void onPublishAcknowledged(OnPublishAck callback);

    /// Publica evento de conectividad (online/offline)
    uint16_t publishEvent(const char* eventJson);

    WiFiClientSecure& getClient() { return _client; }

private:
    WiFiClientSecure& _client;
    PubSubClient _mqtt;
    OnPublishAck _onPublishAck = nullptr;

    const char* _host = nullptr;
    uint16_t _port = 8883;
    const char* _username = nullptr;
    const char* _password = nullptr;
    const char* _clientId = nullptr;
    const char* _lwtTopic = nullptr;
    const char* _lwtPayload = nullptr;

    unsigned long _lastConnectAttempt = 0;
    static constexpr unsigned long RECONNECT_COOLDOWN_MS = 5000;

    bool _loadCACertificate(const char* path);
    static void _mqttCallback(char* topic, byte* payload, unsigned int length);
};

#endif // MQTT_MANAGER_H
