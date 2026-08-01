#ifndef CORE_RELOJ_H
#define CORE_RELOJ_H

#include <cstdint>
#include <ctime>
#include <string>

namespace core {

/**
 * Reloj de pared aproximado construido sobre un contador monótono de
 * milisegundos (`millis()`), con una base fijada por NTP o, en su defecto, por
 * la hora de compilación.
 *
 * El detalle que obliga a que esto sea una clase con estado y no una resta
 * suelta: `millis()` desborda a los ~49.7 días. `_epochBase + (millis() -
 * _millisBase) / 1000` es correcto mientras la distancia entre ambos instantes
 * quepa en 32 bits, pero un nodo que lleve más de 49 días sin resincronizar
 * NTP ve la diferencia dar la vuelta y el timestamp retrocede casi dos meses de
 * golpe. El backend lo rechazaría por `timestamp_demasiado_antiguo` y el nodo
 * dejaría de registrar sin ningún error visible. `avanzar()` rebasa la
 * referencia en cada llamada, así que solo hay que asumir que las llamadas
 * distan menos de 49 días entre sí —ocurren cada 30 s.
 */
class Reloj {
public:
    /// Fija la base de tiempo (sincronización NTP correcta).
    void fijarBase(time_t epoch, uint32_t millisActual) {
        _epochBase = epoch;
        _millisBase = millisActual;
        _restoMs = 0;
        _sincronizado = true;
    }

    /// Fija la base sin marcarla como sincronizada (fallback de compilación).
    void fijarBaseNoSincronizada(time_t epoch, uint32_t millisActual) {
        _epochBase = epoch;
        _millisBase = millisActual;
        _restoMs = 0;
        _sincronizado = false;
    }

    bool sincronizado() const { return _sincronizado; }
    bool tieneBase() const { return _epochBase != 0; }

    /// Época UTC actual, rebasando la referencia para no depender de que la
    /// distancia acumulada quepa en 32 bits.
    time_t avanzar(uint32_t millisActual) {
        const uint32_t transcurridoMs = (uint32_t)(millisActual - _millisBase) + _restoMs;
        _epochBase += (time_t)(transcurridoMs / 1000);
        _restoMs = transcurridoMs % 1000;
        _millisBase = millisActual;
        return _epochBase;
    }

private:
    time_t _epochBase = 0;
    uint32_t _millisBase = 0;
    uint32_t _restoMs = 0;
    bool _sincronizado = false;
};

/// Formatea una época UTC como ISO 8601 con precisión de segundos:
/// "2026-07-25T12:34:56Z". Es el formato exacto que valida el backend.
std::string formatearISO8601(time_t epoch);

/// Convierte `__DATE__` ("Jul 25 2026") y `__TIME__` ("12:34:56") en época UTC.
/// Devuelve 0 si no se pueden interpretar.
time_t epocaDeCompilacion(const char* fecha, const char* hora);

}  // namespace core

#endif  // CORE_RELOJ_H
