#ifndef PAYLOAD_BUILDER_H
#define PAYLOAD_BUILDER_H

#include <Arduino.h>

#include "../core/PayloadCore.h"

/**
 * Construye el payload JSON conforme al esquema validado por Pydantic v2
 * en el backend: LecturaPayload(device_id, timestamp, schema_version,
 * reading_id, estado_conectividad, firmware_version, temperatura_interna,
 * temperatura_ambiental, humedad_ambiental, estado_temperatura_interna,
 * estado_temperatura_ambiental, estado_humedad_ambiental,
 * apertura_refrigerador, mc38_status, duracion_apertura_segundos).
 *
 * Esta clase es solo el envoltorio Arduino: la serialización y sus reglas
 * (null explícito en vez de 0.0, techo de 512 bytes, escapado) viven en
 * `core::serializarLectura()`, que se compila y se prueba en el host. Ver
 * `core/PayloadCore.h` para el contrato y `test/test_core/` para su cobertura.
 */
class PayloadBuilder {
public:
    PayloadBuilder(const char* deviceId, const char* firmwareVersion);

    /// Las lecturas inválidas se pasan como NAN y se serializan como `null`.
    /// `estado` acompaña el `null` para que el backend distinga sensor caído
    /// (se acepta la lectura) de payload malformado (se rechaza) — HU-15/HU-05.
    void setTemperatureInterna(float tempC, core::EstadoSensor estado = core::EstadoSensor::Ok);
    void setTemperatureAmbiental(float tempC, core::EstadoSensor estado = core::EstadoSensor::Ok);
    void setHumidityAmbiental(float humPct, core::EstadoSensor estado = core::EstadoSensor::Ok);
    void setDoorOpen(bool open, unsigned long durationSec);
    void setConnectivityOnline(bool online);
    /// HU-04: false si este nodo no tiene MC-38 instalado — `apertura_refrigerador`
    /// se serializa como `null` en vez de simular una puerta cerrada inexistente.
    void setMc38Instalado(bool instalado);

    /// Serializa a string JSON. Retorna "" si el tamaño supera `maxBytes`.
    String build(unsigned int maxBytes = 512);

    /// Fecha/hora UTC en ISO 8601. El ESP32 no tiene RTC con batería: usa la
    /// base fijada por NTP y, mientras esta no exista, la hora de compilación.
    static String timestampISO8601();

private:
    core::Lectura _lectura;
};

#endif  // PAYLOAD_BUILDER_H
