#ifndef SHT31_SENSOR_H
#define SHT31_SENSOR_H

#include <Arduino.h>
#include <Adafruit_SHT31.h>

/**
 * Sensor SHT31-DIS — temperatura y humedad ambiental del refrigerador.
 *
 * Bus I2C (SDA=GPIO21, SCL=GPIO22 por defecto en ESP32).
 * Precisión: ±0.2 °C temperatura, ±2% HR humedad.
 *
 * Las lecturas de temperatura y humedad se obtienen en una sola llamada I2C.
 * Si cualquiera de las dos es NAN, ambas se marcan como inválidas.
 */
class SHT31Sensor {
public:
    explicit SHT31Sensor(uint8_t i2cAddress = 0x44);
    bool begin();
    float readTemperatureC();
    float readHumidity();

    bool isConnected() const { return _connected; }

private:
    /// Reintenta la deteccion I2C si el sensor se marco como caido.
    bool _reintentarSiCaido();

    Adafruit_SHT31 _sht;
    bool _connected = false;
    static constexpr float MIN_TEMP = -40.0f;
    static constexpr float MAX_TEMP = 125.0f;
};

#endif // SHT31_SENSOR_H
