#ifndef PAYLOAD_BUILDER_H
#define PAYLOAD_BUILDER_H

#include <Arduino.h>
#include <ArduinoJson.h>

/**
 * Construye el payload JSON conforme al esquema validado por Pydantic v2
 * en el backend: LecturaPayload(device_id, timestamp, estado_conectividad,
 * firmware_version, temperatura_interna, temperatura_ambiental,
 * humedad_ambiental, apertura_refrigerador, ...).
 *
 * Campos obligatorios: device_id, timestamp (ISO 8601 UTC), estado_conectividad,
 *                       firmware_version, temperatura_interna
 *
 * Campos que pueden ser null: temperatura_ambiental, humedad_ambiental
 *
 * El JSON se serializa con ArduinoJson v7. Tamaño típico: ~200-250 bytes.
 */
class PayloadBuilder {
public:
    PayloadBuilder(const char* deviceId, const char* firmwareVersion);

    void setTemperatureInterna(float tempC);
    void setTemperatureAmbiental(float tempC);
    void setHumidityAmbiental(float humPct);
    void setDoorOpen(bool open, unsigned long durationSec);
    void setConnectivityOnline(bool online);

    /// Serializa a string JSON. Retorna "" si size > maxBytes.
    String build(unsigned int maxBytes = 512);

    /// Genera la fecha/hora UTC formateada. El ESP32 no tiene RTC real,
    /// así que usa el tiempo de compilación + millis() como aproximación
    /// hasta que se sincronice vía NTP.
    static String timestampISO8601();

private:
    const char* _deviceId;
    const char* _firmwareVersion;
    float _tempInterna = NAN;
    float _tempAmbiental = NAN;
    float _humedad = NAN;
    bool _doorOpen = false;
    unsigned long _doorDurationSec = 0;
    bool _online = false;

    bool _hasTempInterna = false;
    bool _hasTempAmbiental = false;
    bool _hasHumedad = false;
};

#endif // PAYLOAD_BUILDER_H
