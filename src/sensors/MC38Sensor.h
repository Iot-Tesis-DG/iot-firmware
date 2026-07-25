#ifndef MC38_SENSOR_H
#define MC38_SENSOR_H

#include <Arduino.h>

/**
 * Sensor MC-38 — contacto magnético de puerta (reed switch).
 *
 * Conectado a un pin GPIO con pull-up interno.
 * LOW  = puerta cerrada (imán presente, contacto cerrado).
 * HIGH = puerta abierta (imán ausente, contacto abierto).
 *
 * Incluye debounce por hardware: solo se reporta un cambio de estado
 * si la señal se mantiene estable durante DEBOUNCE_MS milisegundos.
 * Además, cuenta cuánto tiempo lleva la puerta abierta.
 */
class MC38Sensor {
public:
    explicit MC38Sensor(uint8_t pin);

    void begin();

    /// true = puerta abierta, false = cerrada. Con debounce.
    bool isOpen();

    /// Segundos que la puerta lleva abierta. 0 si está cerrada.
    unsigned long openDurationSec() const;

private:
    uint8_t _pin;
    bool _currentState = false;       // false = cerrada
    bool _lastStableState = false;
    unsigned long _lastChangeTime = 0;
    unsigned long _openStartTime = 0;

    static constexpr unsigned long DEBOUNCE_MS = 50;   // 50 ms anti-rebote
};

#endif // MC38_SENSOR_H
