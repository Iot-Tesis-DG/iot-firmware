#ifndef CORE_BACKOFF_H
#define CORE_BACKOFF_H

#include <cstdint>

namespace core {

/**
 * Backoff exponencial con tope, sin dependencias de Arduino.
 *
 * Se extrajo de `WiFiManager` por dos razones. La primera es que la política de
 * reintentos (HU-08) era una de las pocas piezas de lógica del firmware con un
 * comportamiento verificable sin hardware y no había forma de ejercitarla: el
 * cálculo vivía entre llamadas bloqueantes a `WiFi.begin()`. La segunda es que
 * el valor inicial estaba duplicado —`WIFI_RECONNECT_BASE_MS` en `config.h` y un
 * `1000` literal en `WiFiManager.h`— y nada garantizaba que coincidieran.
 *
 * `debeIntentar()` usa resta sin signo sobre `millis()`, correcta a través del
 * desbordamiento a los ~49.7 días.
 */
class Backoff {
public:
    Backoff(uint32_t baseMs, uint32_t maxMs, uint32_t factor)
        : _baseMs(baseMs), _maxMs(maxMs), _factor(factor < 2 ? 2 : factor),
          _actualMs(baseMs) {}

    /// Retardo vigente antes del próximo intento.
    uint32_t retardoMs() const { return _actualMs; }

    /// Intentos fallidos consecutivos.
    uint32_t intentos() const { return _intentos; }

    /// ¿Ya venció el retardo? `ahoraMs` y `ultimoIntentoMs` en la misma base
    /// de tiempo (millis()). La resta sin signo sobrevive al desbordamiento.
    bool debeIntentar(uint32_t ahoraMs, uint32_t ultimoIntentoMs) const {
        return (uint32_t)(ahoraMs - ultimoIntentoMs) >= _actualMs;
    }

    /// Registra un fallo: duplica el retardo hasta el tope.
    void registrarFallo() {
        _intentos++;
        // Se comprueba el desbordamiento ANTES de multiplicar: con un tope de
        // 60 s no llega a ocurrir, pero un `_maxMs` mayor y un uint32_t sí.
        if (_actualMs > _maxMs / _factor) {
            _actualMs = _maxMs;
        } else {
            _actualMs *= _factor;
            if (_actualMs > _maxMs) _actualMs = _maxMs;
        }
    }

    /// Registra un éxito: vuelve al retardo base.
    void registrarExito() {
        _actualMs = _baseMs;
        _intentos = 0;
    }

private:
    uint32_t _baseMs;
    uint32_t _maxMs;
    uint32_t _factor;
    uint32_t _actualMs;
    uint32_t _intentos = 0;
};

}  // namespace core

#endif  // CORE_BACKOFF_H
