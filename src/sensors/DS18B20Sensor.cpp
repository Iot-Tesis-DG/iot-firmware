#include "DS18B20Sensor.h"

#include "../config.h"
#include "../core/RangosSensores.h"
#include "../system/Watchdog.h"

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
    // Reintento de detección.
    //
    // Antes, `_connected = false` era definitivo: un único timeout del bus o
    // una lectura de error dejaba el sensor apagado hasta reiniciar el ESP32,
    // porque `begin()` solo se llama en `setup()`. Y este es el sensor que va
    // junto al medicamento: el sistema seguiría publicando
    // `temperatura_interna: null` indefinidamente, sin alerta, porque el guard
    // de sensores del backend trata el null como "sin dato", no como avería.
    //
    // Un roce en el conector 1-Wire no puede costar la variable crítica del
    // experimento, así que se reintenta la detección en cada ciclo (30 s).
    if (!_connected) {
        _dallas.begin();
        if (_dallas.getDeviceCount() == 0) return NAN;
        _dallas.setResolution(12);
        _dallas.setWaitForConversion(false);
        _connected = true;
        LOG_I("DS18B20", "Sensor recuperado tras fallo previo.");
    }

    _dallas.requestTemperatures();           // Inicia conversión asíncrona en todos los sensores
    const unsigned long inicio = millis();
    while (!_dallas.isConversionComplete()) {
        if (millis() - inicio > SENSOR_READ_TIMEOUT_MS) {
            LOG_E("DS18B20", "Timeout en conversión de temperatura.");
            _connected = false;
            return NAN;
        }
        // El sondeo puede llegar a los 2 s (SENSOR_READ_TIMEOUT_MS) y esta
        // tarea está suscrita al watchdog: hay que alimentarlo aquí dentro o el
        // propio timeout del sensor provocaría un reinicio del nodo.
        alimentarWatchdog();
        delay(10);
    }

    const float temp = _dallas.getTempCByIndex(0);

    // -127 °C es el código de error de DallasTemperature cuando el sensor
    // no responde (desconectado, cortocircuito, o bus roto).
    if (core::esFalloDS18B20(temp)) {
        LOG_E("DS18B20", "Sensor no responde (valor de error: %.2f).", temp);
        _connected = false;
        return NAN;
    }

    // Fuera del rango físico de la hoja de datos: la lectura es basura del bus,
    // pero el sensor sigue respondiendo, así que no se marca como caído.
    const float validada = core::validarDS18B20(temp);
    if (isnan(validada)) {
        LOG_E("DS18B20", "Lectura fuera de rango físico: %.2f °C.", temp);
        return NAN;
    }

    LOG_I("DS18B20", "Temperatura interna: %.2f °C", validada);
    return validada;
}
