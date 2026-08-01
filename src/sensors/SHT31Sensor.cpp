#include "SHT31Sensor.h"

#include <Wire.h>

#include "../config.h"
#include "../core/RangosSensores.h"

SHT31Sensor::SHT31Sensor(uint8_t i2cAddress)
    : _sht() {
    // El constructor de Adafruit_SHT31 no acepta dirección I2C.
    // La dirección se configura en begin().
    (void)i2cAddress;
}

bool SHT31Sensor::begin() {
    Wire.begin();  // SDA=GPIO21, SCL=GPIO22 (por defecto en ESP32)

    if (!_sht.begin(SHT31_I2C_ADDRESS)) {
        LOG_E("SHT31", "No se detectó el sensor en 0x%02X. Verificar cableado I2C.", SHT31_I2C_ADDRESS);
        _connected = false;
        return false;
    }

    _connected = true;
    LOG_I("SHT31", "Sensor SHT31-DIS inicializado en 0x%02X.", SHT31_I2C_ADDRESS);
    return true;
}

/// Reintenta detectar el sensor si se marcó como caído.
///
/// `begin()` solo se llama en `setup()`. Si el SHT31 no respondía en ese
/// instante —alimentación aún estabilizándose, un dupont flojo— quedaba
/// descartado para siempre y el nodo publicaba `temperatura_ambiental: null` y
/// `humedad_ambiental: null` durante toda la sesión sin volver a intentarlo.
bool SHT31Sensor::_reintentarSiCaido() {
    if (_connected) return true;
    if (!_sht.begin(SHT31_I2C_ADDRESS)) return false;
    _connected = true;
    LOG_I("SHT31", "Sensor recuperado tras fallo previo.");
    return true;
}

float SHT31Sensor::readTemperatureC() {
    if (!_reintentarSiCaido()) return NAN;

    float t = _sht.readTemperature();
    if (isnan(t)) {
        LOG_E("SHT31", "Fallo en lectura de temperatura.");
        return NAN;
    }

    const float validada = core::validarSHT31Temperatura(t);
    if (isnan(validada)) {
        LOG_E("SHT31", "Temperatura fuera de rango: %.2f °C.", t);
        return NAN;
    }

    LOG_I("SHT31", "Temperatura ambiental: %.2f °C", validada);
    return validada;
}

float SHT31Sensor::readHumidity() {
    if (!_reintentarSiCaido()) return NAN;

    float h = _sht.readHumidity();
    if (isnan(h)) {
        LOG_E("SHT31", "Fallo en lectura de humedad.");
        return NAN;
    }

    const float validada = core::validarSHT31Humedad(h);
    if (isnan(validada)) {
        LOG_E("SHT31", "Humedad fuera de rango: %.2f %%HR.", h);
        return NAN;
    }

    LOG_I("SHT31", "Humedad relativa: %.2f %%HR", validada);
    return validada;
}
