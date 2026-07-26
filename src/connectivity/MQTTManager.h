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
 * - Publicación en QoS 0: PubSubClient no implementa QoS 1 al publicar, así
 *   que no hay PUBACK ni packetId. El LWT sí se registra en QoS 1.
 * - El buffer LittleFS se libera tras publicar y revalidar la sesión.
 */
class MQTTManager {
public:

    MQTTManager(WiFiClientSecure& tlsClient);

    void begin(const char* host, uint16_t port,
               const char* username, const char* password,
               const char* clientId);

    bool connect();
    bool isConnected();
    bool loop();  // Debe llamarse frecuentemente para procesar callbacks

    /// Publica un payload JSON. Retorna packetId (>0) si se encoló para envío.
    /// QoS 0: devuelve 1 si el paquete salió por el socket, no si el broker
    /// lo recibió. Ver la nota de garantías en MQTTManager.cpp.
    uint16_t publish(const char* topic, const char* payload, bool retained = false);

    /// Publica evento de conectividad (online/offline)
    uint16_t publishEvent(const char* eventJson);

    WiFiClientSecure& getClient() { return _client; }

private:
    WiFiClientSecure& _client;
    PubSubClient _mqtt;

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
