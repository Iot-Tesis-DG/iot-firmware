#include "MC38Sensor.h"
#include "../config.h"

MC38Sensor::MC38Sensor(uint8_t pin)
    : _pin(pin) {}

void MC38Sensor::begin() {
    pinMode(_pin, INPUT_PULLUP);

    const bool abierta = (digitalRead(_pin) == HIGH);
    _estadoEstable = abierta;
    _ultimaMuestra = abierta;
    _muestraDesde = millis();

    if (abierta) {
        _abiertaDesde = millis();
        _huboApertura = true;
    }

    LOG_I("MC38", "Sensor de puerta inicializado en GPIO%d. Estado: %s.",
          _pin, abierta ? "ABIERTA" : "cerrada");
}

void MC38Sensor::poll() {
    const bool cruda = (digitalRead(_pin) == HIGH);
    const unsigned long ahora = millis();

    // El temporizador de estabilidad se reinicia cuando cambia la MUESTRA, no
    // cuando la muestra coincide con el estado estable. La versión anterior
    // hacía lo segundo, así que el temporizador se ponía a cero en cada lectura
    // tranquila y el umbral de 50 ms no llegaba a aplicarse nunca.
    if (cruda != _ultimaMuestra) {
        _ultimaMuestra = cruda;
        _muestraDesde = ahora;
        return;
    }

    // Muestra repetida: ¿lleva estable lo suficiente y difiere del estado?
    if (cruda == _estadoEstable) return;
    if (ahora - _muestraDesde < DEBOUNCE_MS) return;

    _estadoEstable = cruda;

    if (cruda) {
        _abiertaDesde = ahora;
        _huboApertura = true;
        LOG_I("MC38", "Puerta ABIERTA.");
    } else {
        if (_abiertaDesde != 0) {
            _acumuladoMs += (ahora - _abiertaDesde);
            _abiertaDesde = 0;
        }
        LOG_I("MC38", "Puerta cerrada.");
    }
}

bool MC38Sensor::isOpen() const {
    return _estadoEstable;
}

bool MC38Sensor::huboApertura() const {
    return _huboApertura;
}

unsigned long MC38Sensor::duracionAperturaSegundos() const {
    unsigned long total = _acumuladoMs;
    if (_abiertaDesde != 0) {
        total += (millis() - _abiertaDesde);  // apertura todavía en curso
    }
    return total / 1000;
}

void MC38Sensor::limpiarReporte() {
    _acumuladoMs = 0;
    // Si sigue abierta, la siguiente ventana cuenta desde ahora y el indicador
    // de apertura se mantiene: la puerta está abierta *también* en esa ventana.
    if (_abiertaDesde != 0) {
        _abiertaDesde = millis();
        _huboApertura = true;
    } else {
        _huboApertura = false;
    }
}
