#include "PayloadBuilder.h"
#include "../config.h"
#include <sys/time.h>

// =========================================================================
// Tiempo UTC aproximado.
//
// El ESP32 no tiene RTC con batería. Usamos dos estrategias:
//   1. NTP al arrancar (configurable, ver main.cpp).
//   2. Si NTP falla, usamos __TIME__ / __DATE__ de compilación + millis().
//
// El backend ya valida timestamps con ventana de ±2h (B-10), así que
// un desfase de minutos no rompe nada. Si el backend rechaza la lectura
// por timestamp inválido, se audita y se descarta.
// =========================================================================

static time_t _epochBase = 0;       // Época base (NTP o compilación)
static unsigned long _millisBase = 0; // millis() cuando se fijó _epochBase
static bool _ntpSynced = false;

/// Sincroniza reloj vía NTP. Llamar en setup().
void syncNTP() {
    configTime(0, 0, "pool.ntp.org", "time.nist.gov", "time.google.com");
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 10000)) {  // 10s timeout
        time_t now;
        time(&now);
        _epochBase = now;
        _millisBase = millis();
        _ntpSynced = true;
        LOG_I("NTP", "Hora sincronizada: %04d-%02d-%02dT%02d:%02d:%02dZ",
              timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
              timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    } else {
        LOG_E("NTP", "No se pudo sincronizar. Usando hora de compilación.");
    }
}

/// ¿El reloj se sincronizó por NTP alguna vez?
/// Sin esto no hay forma de saber si los timestamps salen de NTP o del
/// fallback de compilación, que se desvía y acaba siendo rechazado por el
/// backend.
bool ntpEstaSincronizado() {
    return _ntpSynced;
}

String PayloadBuilder::timestampISO8601() {
    time_t now;
    if (_ntpSynced) {
        now = _epochBase + ((millis() - _millisBase) / 1000);
    } else {
        // Fallback: hora de compilación + uptime
        if (_epochBase == 0) {
            // Inicializado a cero de forma explícita: `mktime()` lee también
            // `tm_isdst`, y dejarlo con basura de pila desplazaba la hora una
            // hora entera —o devolvía -1— de forma no determinista.
            struct tm tm_compile = {};
            // __DATE__ = "Jul 25 2026", __TIME__ = "12:34:56"
            char month[4];
            int day, year, hour, min, sec;
            sscanf(__DATE__, "%s %d %d", month, &day, &year);
            sscanf(__TIME__, "%d:%d:%d", &hour, &min, &sec);
            const char* months[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                    "Jul","Aug","Sep","Oct","Nov","Dec"};
            tm_compile.tm_mon = 0;
            for (int i = 0; i < 12; i++) {
                if (strcmp(month, months[i]) == 0) { tm_compile.tm_mon = i; break; }
            }
            tm_compile.tm_mday = day;
            tm_compile.tm_year = year - 1900;
            tm_compile.tm_hour = hour;
            tm_compile.tm_min = min;
            tm_compile.tm_sec = sec;
            _epochBase = mktime(&tm_compile);
            _millisBase = millis();
        }
        now = _epochBase + ((millis() - _millisBase) / 1000);
    }

    struct tm* utc = gmtime(&now);
    char buf[30];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
             utc->tm_year + 1900, utc->tm_mon + 1, utc->tm_mday,
             utc->tm_hour, utc->tm_min, utc->tm_sec);
    return String(buf);
}

// =========================================================================
// PayloadBuilder
// =========================================================================

PayloadBuilder::PayloadBuilder(const char* deviceId, const char* firmwareVersion)
    : _deviceId(deviceId), _firmwareVersion(firmwareVersion) {}

void PayloadBuilder::setTemperatureInterna(float tempC) {
    _tempInterna = tempC;
    _hasTempInterna = !isnan(tempC);
}

void PayloadBuilder::setTemperatureAmbiental(float tempC) {
    _tempAmbiental = tempC;
    _hasTempAmbiental = !isnan(tempC);
}

void PayloadBuilder::setHumidityAmbiental(float humPct) {
    _humedad = humPct;
    _hasHumedad = !isnan(humPct);
}

void PayloadBuilder::setDoorOpen(bool open, unsigned long durationSec) {
    _doorOpen = open;
    _doorDurationSec = durationSec;
}

void PayloadBuilder::setConnectivityOnline(bool online) {
    _online = online;
}

String PayloadBuilder::build(unsigned int maxBytes) {
    JsonDocument doc;

    doc["device_id"] = _deviceId;
    doc["timestamp"] = timestampISO8601();
    doc["estado_conectividad"] = _online ? "online" : "offline";
    doc["firmware_version"] = _firmwareVersion;

    // =========================================================================
    // Sensores: enviar como number o como null (no omitir campos).
    // El backend espera exactamente estos nombres de campo (Pydantic v2).
    // HU-05 Escenario 2: "El campo se serializa explícitamente como null
    // con bandera de error, en vez de omitirse."
    // =========================================================================
    if (_hasTempInterna) {
        doc["temperatura_interna"] = _tempInterna;
    } else {
        doc["temperatura_interna"] = nullptr;
    }

    if (_hasTempAmbiental) {
        doc["temperatura_ambiental"] = _tempAmbiental;
    } else {
        doc["temperatura_ambiental"] = nullptr;
    }

    if (_hasHumedad) {
        doc["humedad_ambiental"] = _humedad;
    } else {
        doc["humedad_ambiental"] = nullptr;
    }

    doc["apertura_refrigerador"] = _doorOpen;

    if (_doorOpen && _doorDurationSec > 0) {
        doc["duracion_apertura_segundos"] = _doorDurationSec;
    } else {
        doc["duracion_apertura_segundos"] = 0;
    }

    // Serializar
    String output;
    size_t len = serializeJson(doc, output);

    if (len > maxBytes) {
        LOG_E("Payload", "JSON demasiado grande: %d bytes (máx %d).", len, maxBytes);
        return "";
    }

    LOG_I("Payload", "JSON construido: %d bytes.", len);
    return output;
}
