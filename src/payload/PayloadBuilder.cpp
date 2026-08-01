#include "PayloadBuilder.h"

#include <sys/time.h>

#include "../config.h"
#include "../core/PayloadCore.h"
#include "../core/Reloj.h"

// =========================================================================
// Reloj compartido entre núcleos.
//
// `syncNTP()` la escribe desde el Core 1 (taskRed reintenta la sincronización
// en cada reconexión) y `timestampISO8601()` la lee desde el Core 0 cada 30 s.
// Antes eran tres variables estáticas sueltas —`time_t`, `unsigned long` y
// `bool`— sin ninguna sincronización: un lector podía ver la época nueva con la
// referencia de `millis()` vieja y emitir un timestamp desplazado por el
// uptime completo del nodo, justo en el instante en que se recupera la red.
//
// Se protege con un spinlock de FreeRTOS (`portMUX_TYPE`): la sección crítica
// son unas pocas operaciones aritméticas, así que un mutex con bloqueo sería
// más caro que la propia operación. Ninguna llamada bloqueante entra dentro.
// =========================================================================
static core::Reloj _reloj;
static portMUX_TYPE _relojMux = portMUX_INITIALIZER_UNLOCKED;

void syncNTP() {
    configTime(0, 0, "pool.ntp.org", "time.nist.gov", "time.google.com");
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, NTP_SYNC_TIMEOUT_MS)) {  // FUERA de la sección crítica
        time_t ahora;
        time(&ahora);
        const uint32_t marca = (uint32_t)millis();

        portENTER_CRITICAL(&_relojMux);
        _reloj.fijarBase(ahora, marca);
        portEXIT_CRITICAL(&_relojMux);

        LOG_I("NTP", "Hora sincronizada: %s", core::formatearISO8601(ahora).c_str());
    } else {
        LOG_E("NTP", "No se pudo sincronizar. Usando hora de compilación.");
    }
}

bool ntpEstaSincronizado() {
    portENTER_CRITICAL(&_relojMux);
    const bool ok = _reloj.sincronizado();
    portEXIT_CRITICAL(&_relojMux);
    return ok;
}

String PayloadBuilder::timestampISO8601() {
    const uint32_t marca = (uint32_t)millis();

    portENTER_CRITICAL(&_relojMux);
    if (!_reloj.tieneBase()) {
        // Sin NTP todavía: se ancla a la hora de compilación. El backend valida
        // una ventana de ±2 h, así que esto solo sirve para las primeras horas
        // tras el flasheo; `taskRed` reintenta NTP en cada reconexión.
        _reloj.fijarBaseNoSincronizada(core::epocaDeCompilacion(__DATE__, __TIME__), marca);
    }
    const time_t ahora = _reloj.avanzar(marca);
    portEXIT_CRITICAL(&_relojMux);

    return String(core::formatearISO8601(ahora).c_str());
}

// =========================================================================
// PayloadBuilder — envoltorio Arduino sobre core::serializarLectura().
// =========================================================================

PayloadBuilder::PayloadBuilder(const char* deviceId, const char* firmwareVersion) {
    _lectura.deviceId = deviceId != nullptr ? deviceId : "";
    _lectura.firmwareVersion = firmwareVersion != nullptr ? firmwareVersion : "";
}

void PayloadBuilder::setTemperatureInterna(float tempC) {
    _lectura.temperaturaInterna = tempC;
}

void PayloadBuilder::setTemperatureAmbiental(float tempC) {
    _lectura.temperaturaAmbiental = tempC;
}

void PayloadBuilder::setHumidityAmbiental(float humPct) {
    _lectura.humedadAmbiental = humPct;
}

void PayloadBuilder::setDoorOpen(bool open, unsigned long durationSec) {
    _lectura.aperturaRefrigerador = open;
    _lectura.duracionAperturaSegundos = (uint32_t)durationSec;
}

void PayloadBuilder::setConnectivityOnline(bool online) {
    _lectura.online = online;
}

String PayloadBuilder::build(unsigned int maxBytes) {
    _lectura.timestamp = std::string(timestampISO8601().c_str());

    const std::string json = core::serializarLectura(_lectura, maxBytes);
    if (json.empty()) {
        LOG_E("Payload", "JSON descartado: excede el maximo de %u bytes.", maxBytes);
        return String();
    }

    LOG_I("Payload", "JSON construido: %u bytes.", (unsigned)json.size());
    return String(json.c_str());
}
