#include "DS18B20Sensor.h"
#include "../config.h"

DS18B20Sensor::DS18B20Sensor(uint8_t pin)
    : _oneWire(pin), _dallas(&_oneWire) {}

void DS18B20Sensor::begin() {
    _dallas.begin();
    int count = _dallas.getDeviceCount();
    LOG_I("DS18B20", "Dispositivos 1-Wire encontrados: %d", count);

    if (count > 0) {
        _dallas.setResolution(12);          // 12 bits = ±0.0625 °C, ~750 ms conversión
        _dallas.setWaitForConversion(false); // No bloqueante: pedimos lectura y seguimos
        _connected = true;
    } else {
        LOG_E("DS18B20", "No se detectó el sensor. Verificar cableado y pull-up de 4.7kΩ.");
        _connected = false;
    }
}

float DS18B20Sensor::readTemperatureC() {
    if (!_connected) return NAN;

    _dallas.requestTemperatures();           // Inicia conversión asíncrona en todos los sensores
    unsigned long inicio = millis();
    while (!_dallas.isConversionComplete()) {
        if (millis() - inicio > SENSOR_READ_TIMEOUT_MS) {
            LOG_E("DS18B20", "Timeout en conversión de temperatura.");
            _connected = false;
            return NAN;
        }
        delay(10);
    }

    float temp = _dallas.getTempCByIndex(0);

    // -127 °C es el código de error de DallasTemperature cuando el sensor
    // no responde (desconectado, cortocircuito, o bus roto).
    if (temp <= ERROR_VALUE) {
        LOG_E("DS18B20", "Sensor no responde (valor de error: %.2f).", temp);
        _connected = false;
        return NAN;
    }

    if (temp < MIN_VALID || temp > MAX_VALID) {
        LOG_E("DS18B20", "Lectura fuera de rango físico: %.2f °C.", temp);
        return NAN;
    }

    LOG_I("DS18B20", "Temperatura interna: %.2f °C", temp);
    return temp;
}
