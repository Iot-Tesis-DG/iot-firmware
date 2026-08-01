#include "WiFiManager.h"

#include "../config.h"
#include "../system/Watchdog.h"

#include "../core/Credenciales.h"

void WiFiManager::configurar(const String& ssid, const String& password) {
    _ssid = ssid;
    _password = password;
}

bool WiFiManager::tieneCredenciales() const {
    return core::credencialValida(std::string(_ssid.c_str()));
}

void WiFiManager::begin() {
    if (!tieneCredenciales()) {
        LOG_E("WiFi", "Sin SSID aprovisionado: el nodo opera solo offline.");
        return;
    }
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(false);  // Control manual de reconexión
    _lastAttemptTime = millis();
    _connect();
}

bool WiFiManager::isConnected() const {
    return WiFi.status() == WL_CONNECTED;
}

bool WiFiManager::maintain() {
    if (isConnected()) {
        return true;
    }
    if (!tieneCredenciales()) return false;

    const unsigned long ahora = millis();
    if (!_backoff.debeIntentar((uint32_t)ahora, (uint32_t)_lastAttemptTime)) {
        return false;  // Esperando el backoff
    }

    _lastAttemptTime = ahora;
    LOG_I("WiFi", "Intento de reconexión #%d (backoff: %lu ms)...",
          reconnectAttempts() + 1, reconnectDelayMs());
    return _connect();
}

bool WiFiManager::_connect() {
    // `disconnect()` antes de reintentar: llamar a `WiFi.begin()` sobre una
    // sesión a medio establecer deja al supplicant en un estado desde el que no
    // reintenta, y el nodo se quedaba en WL_DISCONNECTED hasta el reinicio.
    WiFi.disconnect(false, false);
    WiFi.begin(_ssid.c_str(), _password.c_str());

    const unsigned long inicio = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - inicio > WIFI_CONNECT_TIMEOUT_MS) {
            LOG_E("WiFi", "Timeout de conexión a '%s'.", _ssid.c_str());
            _backoff.registrarFallo();
            return false;
        }
        // Este bucle bloquea hasta 15 s dentro de una tarea suscrita al
        // watchdog: sin alimentarlo aquí, un AP fuera de alcance reiniciaría el
        // nodo en vez de dejarlo capturando en modo offline.
        alimentarWatchdog();
        delay(500);
    }

    _backoff.registrarExito();
    _wasEverConnected = true;
    LOG_I("WiFi", "Conectado! IP: %s, RSSI: %d dBm.",
          WiFi.localIP().toString().c_str(), WiFi.RSSI());
    return true;
}
