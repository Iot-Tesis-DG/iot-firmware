#ifndef CORE_PAYLOAD_CORE_H
#define CORE_PAYLOAD_CORE_H

#include <cmath>
#include <cstdint>
#include <string>

namespace core {

/// HU-05: versión del contrato de payload que este firmware emite. Debe
/// coincidir con `SCHEMA_VERSIONS_SOPORTADAS` del backend
/// (`src/infrastructure/mqtt/payload_schema.py`).
constexpr int SCHEMA_VERSION_PAYLOAD = 1;

/**
 * Serialización del payload de lectura, sin Arduino ni ArduinoJson.
 *
 * Es el contrato con el backend (`LecturaPayload`, Pydantic v2 con
 * `extra="forbid"` y `allow_inf_nan=False`), y es el punto donde el proyecto ya
 * se rompió una vez: `duracion_apertura_segundos` se emitía y el backend no lo
 * declaraba, así que rechazaba el 100 % de los mensajes del firmware real. No
 * lo detectó ninguna prueba porque todas las del backend construían el payload
 * a mano y las del firmware no existían. Ahora el firmware tiene su propia
 * prueba contra el payload literal documentado en §3.5.
 *
 * Reglas que la implementación garantiza (HU-05):
 *   - Los tres campos de sensor se emiten SIEMPRE. Si la lectura no es válida
 *     se emiten como `null` explícito, nunca se omiten y nunca valen 0.0.
 *   - NaN e infinito jamás se serializan como número: el backend los rechaza
 *     con `allow_inf_nan=False` y perdería la lectura entera.
 *   - `escaparJSON` sanea `device_id` y `firmware_version` por si llegaran con
 *     comillas o barras desde un `build_flag`, que rompería el JSON.
 *
 * Se serializa a mano en vez de con ArduinoJson porque el documento tiene ocho
 * campos planos y de tipo fijo: no compensa un `JsonDocument` en el heap por
 * cada lectura —fragmenta la RAM del ESP32 en un proceso que corre durante
 * semanas— ni una dependencia que impide compilar esta lógica en el host.
 */
/// HU-15/HU-05: estado explícito de un sensor individual, para que el
/// backend distinga "sensor caído" (se acepta la lectura) de "payload
/// malformado" (se rechaza). "ok" es el único estado válido cuando el valor
/// no es null.
enum class EstadoSensor {
    Ok,
    SensorError,
    FueraDeRango,
};

inline const char* nombreEstadoSensor(EstadoSensor estado) {
    switch (estado) {
        case EstadoSensor::SensorError:  return "sensor_error";
        case EstadoSensor::FueraDeRango: return "fuera_de_rango";
        case EstadoSensor::Ok:
        default:                         return "ok";
    }
}

struct Lectura {
    std::string deviceId;
    std::string firmwareVersion;
    std::string timestamp;   ///< ISO 8601 UTC: "2026-07-25T12:34:56Z"
    std::string readingId;   ///< HU-05: identidad lógica de ESTA lectura (deviceId + timestamp)
    bool online = false;

    float temperaturaInterna = NAN;
    float temperaturaAmbiental = NAN;
    float humedadAmbiental = NAN;
    EstadoSensor estadoTemperaturaInterna = EstadoSensor::Ok;
    EstadoSensor estadoTemperaturaAmbiental = EstadoSensor::Ok;
    EstadoSensor estadoHumedadAmbiental = EstadoSensor::Ok;

    bool aperturaRefrigerador = false;
    uint32_t duracionAperturaSegundos = 0;
    // HU-04: false si este nodo NO tiene MC-38 instalado — en ese caso
    // `aperturaRefrigerador` se serializa como `null`, no como `false`, para
    // no simular una puerta cerrada que en realidad no existe.
    bool mc38Instalado = true;
};

/// Escapa una cadena para incrustarla en JSON entre comillas.
std::string escaparJSON(const std::string& entrada);

/// Serializa la lectura. Devuelve "" si el resultado excede `maxBytes`,
/// porque publicar un payload truncado es peor que saltarse el ciclo.
std::string serializarLectura(const Lectura& lectura, size_t maxBytes = 512);

/// HU-07: extrae el valor de un campo string de un JSON PLANO de un solo
/// nivel — NO es un parser JSON general (no soporta objetos/arreglos
/// anidados ni comillas escapadas dentro del valor). Es lo mínimo para leer
/// `reading_id` del propio payload que este firmware generó y del acuse
/// lógico que el backend publica de vuelta (ambos con esa forma), sin traer
/// una dependencia completa de JSON solo para un campo. Devuelve "" si el
/// campo no está presente.
std::string extraerCampoString(const std::string& json, const std::string& campo);

}  // namespace core

#endif  // CORE_PAYLOAD_CORE_H
