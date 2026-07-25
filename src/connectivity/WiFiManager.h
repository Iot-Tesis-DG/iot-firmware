#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <Arduino.h>
#include <WiFi.h>

/**
 * Gestión de conexión Wi-Fi con backoff exponencial.
 *
 * Se ejecuta en el Core 1 (junto con MQTT) para no bloquear
 * la captura de sensores en el Core 0.
 *
 * Backoff: 1s → 2s → 4s → 8s → 16s → ... → 60s tope.
 * Al reconectar exitosamente, el contador se reinicia.
 */
class WiFiManager {
public:
    WiFiManager(const char* ssid, const char* password);

    void begin();
    bool isConnected() const;
    bool maintain();        // Llamar en loop: reconecta si es necesario

    unsigned long reconnectDelayMs() const { return _backoffMs; }
    int reconnectAttempts() const { return _attempts; }

private:
    const char* _ssid;
    const char* _password;
    unsigned long _backoffMs = 1000;
    int _attempts = 0;
    unsigned long _lastAttemptTime = 0;
    bool _wasEverConnected = false;

    bool _connect();
    void _resetBackoff();
    void _increaseBackoff();
};

#endif // WIFI_MANAGER_H
