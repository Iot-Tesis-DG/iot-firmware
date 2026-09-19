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

void PayloadBuilder::setTemperatureInterna(float tempC, core::EstadoSensor estado) {
    _lectura.temperaturaInterna = tempC;
    _lectura.estadoTemperaturaInterna = estado;
}

void PayloadBuilder::setTemperatureAmbiental(float tempC, core::EstadoSensor estado) {
    _lectura.temperaturaAmbiental = tempC;
    _lectura.estadoTemperaturaAmbiental = estado;
}

void PayloadBuilder::setHumidityAmbiental(float humPct, core::EstadoSensor estado) {
    _lectura.humedadAmbiental = humPct;
    _lectura.estadoHumedadAmbiental = estado;
}

void PayloadBuilder::setDoorOpen(bool open, unsigned long durationSec) {
    _lectura.aperturaRefrigerador = open;
    _lectura.duracionAperturaSegundos = (uint32_t)durationSec;
}

void PayloadBuilder::setConnectivityOnline(bool online) {
    _lectura.online = online;
}

void PayloadBuilder::setMc38Instalado(bool instalado) {
    _lectura.mc38Instalado = instalado;
}

// HU-01/HU-11: bootId se fija una sola vez desde setup() (single-writer antes
// de crear las tareas, mismo patrón que las credenciales); seqNo es un
// contador de proceso que solo taskSensores incrementa —un único llamador,
// sin necesidad de sección crítica— y que reinicia en 0 al reiniciar el nodo,
// exactamente la semántica de "monótono dentro de este boot".
static uint32_t _bootIdActual = 0;
static uint32_t _seqNoActual = 0;

void PayloadBuilder::setBootId(uint32_t bootId) {
    _bootIdActual = bootId;
}

String PayloadBuilder::build(unsigned int maxBytes) {
    _lectura.timestamp = std::string(timestampISO8601().c_str());
    // HU-05: reading_id reutiliza device_id+timestamp — ya es la clave de
    // deduplicación de compatibilidad del backend para firmware anterior a
    // boot_id+seq_no. Se recalcula cada vez que se construye (no en el
    // constructor) porque depende del timestamp recién fijado arriba.
    _lectura.readingId = _lectura.deviceId + "_" + _lectura.timestamp;
    _lectura.bootId = _bootIdActual;
    _lectura.seqNo = _seqNoActual++;
    _lectura.tiempoSincronizado = ntpEstaSincronizado();

    const std::string json = core::serializarLectura(_lectura, maxBytes);
    if (json.empty()) {
        LOG_E("Payload", "JSON descartado: excede el maximo de %u bytes.", maxBytes);
        return String();
    }

    LOG_I("Payload", "JSON construido: %u bytes.", (unsigned)json.size());
    return String(json.c_str());
}
