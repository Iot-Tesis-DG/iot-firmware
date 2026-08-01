#include "MC38Sensor.h"

#include "../config.h"

MC38Sensor::MC38Sensor(uint8_t pin) : _pin(pin) {}

void MC38Sensor::begin() {
    pinMode(_pin, INPUT_PULLUP);

    const bool abierta = (digitalRead(_pin) == HIGH);
    _antirrebote.inicializar(abierta, (uint32_t)millis());

    LOG_I("MC38", "Sensor de puerta inicializado en GPIO%d. Estado: %s.",
          _pin, abierta ? "ABIERTA" : "cerrada");
}

void MC38Sensor::poll() {
    const bool cruda = (digitalRead(_pin) == HIGH);
    if (_antirrebote.muestrear(cruda, (uint32_t)millis())) {
        LOG_I("MC38", "Puerta %s.", _antirrebote.estaAbierta() ? "ABIERTA" : "cerrada");
    }
}

bool MC38Sensor::isOpen() const {
    return _antirrebote.estaAbierta();
}

bool MC38Sensor::huboApertura() const {
    return _antirrebote.huboApertura();
}

unsigned long MC38Sensor::duracionAperturaSegundos() const {
    return _antirrebote.duracionSegundos((uint32_t)millis());
}

void MC38Sensor::limpiarReporte() {
    _antirrebote.cerrarVentana((uint32_t)millis());
}
