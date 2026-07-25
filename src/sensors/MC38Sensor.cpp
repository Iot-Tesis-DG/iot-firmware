#include "MC38Sensor.h"
#include "../config.h"

MC38Sensor::MC38Sensor(uint8_t pin)
    : _pin(pin) {}

void MC38Sensor::begin() {
    pinMode(_pin, INPUT_PULLUP);
    _currentState = (digitalRead(_pin) == HIGH);
    _lastStableState = _currentState;
    _lastChangeTime = millis();

    if (_currentState) {
        _openStartTime = millis();
    }

    LOG_I("MC38", "Sensor de puerta inicializado en GPIO%d. Estado: %s.",
          _pin, _currentState ? "ABIERTA" : "cerrada");
}

bool MC38Sensor::isOpen() {
    bool raw = (digitalRead(_pin) == HIGH);

    // Si la lectura coincide con el estado actual, reiniciar temporizador de
    // cambio (no hubo transición).
    if (raw == _currentState) {
        _lastChangeTime = millis();
        return _lastStableState;
    }

    // La señal cambió. Si no ha pasado suficiente tiempo, ignorar (rebote).
    if (millis() - _lastChangeTime < DEBOUNCE_MS) {
        return _lastStableState;
    }

    // Cambio estable confirmado.
    _currentState = raw;
    _lastStableState = raw;
    _lastChangeTime = millis();

    if (raw) {
        _openStartTime = millis();
        LOG_I("MC38", "Puerta ABIERTA.");
    } else {
        _openStartTime = 0;
        LOG_I("MC38", "Puerta cerrada.");
    }

    return _lastStableState;
}

unsigned long MC38Sensor::openDurationSec() const {
    if (!_lastStableState || _openStartTime == 0) return 0;
    return (millis() - _openStartTime) / 1000;
}
