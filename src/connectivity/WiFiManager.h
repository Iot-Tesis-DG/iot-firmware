#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <Arduino.h>
#include <WiFi.h>

#include "../config.h"
#include "../core/Backoff.h"

/**
 * Gestión de conexión Wi-Fi con backoff exponencial (HU-08).
 *
 * Se ejecuta en el Core 1 (junto con MQTT) para no bloquear la captura de
 * sensores en el Core 0.
 *
 * Backoff: 1s → 2s → 4s → 8s → 16s → ... → 60s tope. Al reconectar, el
 * contador se reinicia. La política vive en `core::Backoff`, que se prueba en
 * el host; aquí solo queda el pegamento con el SDK.
 */
class WiFiManager {
public:
    WiFiManager() = default;

    /// Fija las credenciales antes de `begin()`. Se guardan como `String`
    /// propias: llegan de NVS en tiempo de ejecución, no de una macro, así que
    /// el objeto global no puede quedarse con un puntero a memoria ajena.
    void configurar(const String& ssid, const String& password);

    void begin();
    bool tieneCredenciales() const;
    bool isConnected() const;
    bool maintain();  // Llamar en loop: reconecta si es necesario

    unsigned long reconnectDelayMs() const { return _backoff.retardoMs(); }
    int reconnectAttempts() const { return (int)_backoff.intentos(); }

    /// HU-08 criterio 3: true mientras los reintentos fallidos superen
    /// `WIFI_UMBRAL_DIAGNOSTICO_INTENTOS` sin reconectar. La captura offline
    /// (Core 0 / LittleFS) no depende de esto ni se detiene por esto — es
    /// solo la señal observable de que la ausencia de red excede lo normal.
    bool diagnosticoActivo() const { return _diagnosticoActivo; }

    /// Evidencia de un episodio de diagnóstico ya cerrado (reconectado).
    struct RecuperacionDiagnostico {
        int intentosFallidos = 0;
        unsigned long duracionMs = 0;
    };

    /// true una única vez, en la pasada donde el nodo se reconecta DESPUÉS
    /// de haber estado en diagnóstico — para que el llamador (que sí puede
    /// publicar, con la red ya de vuelta) reporte la recuperación una sola
    /// vez y no en cada `maintain()` posterior.
    bool consumirRecuperacionDeDiagnostico(RecuperacionDiagnostico& detalle);

private:
    String _ssid;
    String _password;
    core::Backoff _backoff{WIFI_RECONNECT_BASE_MS, WIFI_RECONNECT_MAX_MS,
                           WIFI_RECONNECT_FACTOR};
    unsigned long _lastAttemptTime = 0;
    bool _wasEverConnected = false;
    bool _diagnosticoActivo = false;
    bool _recuperacionPendiente = false;
    unsigned long _inicioEpisodioDiagnosticoMs = 0;
    RecuperacionDiagnostico _ultimaRecuperacion;

    bool _connect();
};

#endif  // WIFI_MANAGER_H
