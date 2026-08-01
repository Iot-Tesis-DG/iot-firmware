#ifndef DS18B20_SENSOR_H
#define DS18B20_SENSOR_H

#include <Arduino.h>
#include <OneWire.h>
#include <DallasTemperature.h>

/**
 * Sensor DS18B20 — temperatura interna del refrigerador junto al medicamento.
 *
 * Bus 1-Wire (GPIO4), resolución 12 bits (±0.5 °C).
 * El valor de error -127 °C indica sensor desconectado o en falla:
 * en ese caso devuelve NAN en lugar de un número engañoso.
 */
class DS18B20Sensor {
public:
    explicit DS18B20Sensor(uint8_t pin);
    void begin();
    float readTemperatureC();

    /// true si la última lectura fue exitosa (sensor presente y en rango).
    bool isConnected() const { return _connected; }

private:
    OneWire _oneWire;
    DallasTemperature _dallas;
    bool _connected = false;

    // Los umbrales (-127 °C de error, rango físico -55..125 °C) viven en
    // `core/RangosSensores.h`, compartidos con las pruebas del host. Estaban
    // duplicados aquí y en el .cpp, sin nada que garantizara que coincidían.
};

#endif // DS18B20_SENSOR_H
