#include "WiFiManager.h"
#include "../config.h"

WiFiManager::WiFiManager(const char* ssid, const char* password)
    : _ssid(ssid), _password(password) {}

void WiFiManager::begin() {
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(false);  // Control manual de reconexión
    _connect();
}

bool WiFiManager::isConnected() const {
    return WiFi.status() == WL_CONNECTED;
}

bool WiFiManager::maintain() {
    if (isConnected()) {
        return true;
    }

    unsigned long ahora = millis();
    if (ahora - _lastAttemptTime < _backoffMs) {
        return false;  // Esperando el backoff
    }

    _lastAttemptTime = ahora;
    LOG_I("WiFi", "Intento de reconexión #%d (backoff: %lu ms)...", _attempts + 1, _backoffMs);
    return _connect();
}

bool WiFiManager::_connect() {
    _attempts++;
    WiFi.begin(_ssid, _password);

    unsigned long inicio = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - inicio > 15000) {  // 15s timeout
            LOG_E("WiFi", "Timeout de conexión a '%s'.", _ssid);
            _increaseBackoff();
            return false;
        }
        delay(500);
    }

    _resetBackoff();
    _wasEverConnected = true;
    LOG_I("WiFi", "Conectado! IP: %s, RSSI: %d dBm.",
          WiFi.localIP().toString().c_str(), WiFi.RSSI());
    return true;
}

void WiFiManager::_resetBackoff() {
    _backoffMs = WIFI_RECONNECT_BASE_MS;
    _attempts = 0;
}

void WiFiManager::_increaseBackoff() {
    _backoffMs *= WIFI_RECONNECT_FACTOR;
    if (_backoffMs > WIFI_RECONNECT_MAX_MS) {
        _backoffMs = WIFI_RECONNECT_MAX_MS;
    }
}
