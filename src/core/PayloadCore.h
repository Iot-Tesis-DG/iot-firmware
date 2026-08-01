#ifndef CORE_PAYLOAD_CORE_H
#define CORE_PAYLOAD_CORE_H

#include <cmath>
#include <cstdint>
#include <string>

namespace core {

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
struct Lectura {
    std::string deviceId;
    std::string firmwareVersion;
    std::string timestamp;   ///< ISO 8601 UTC: "2026-07-25T12:34:56Z"
    bool online = false;

    float temperaturaInterna = NAN;
    float temperaturaAmbiental = NAN;
    float humedadAmbiental = NAN;

    bool aperturaRefrigerador = false;
    uint32_t duracionAperturaSegundos = 0;
};

/// Escapa una cadena para incrustarla en JSON entre comillas.
std::string escaparJSON(const std::string& entrada);

/// Serializa la lectura. Devuelve "" si el resultado excede `maxBytes`,
/// porque publicar un payload truncado es peor que saltarse el ciclo.
std::string serializarLectura(const Lectura& lectura, size_t maxBytes = 512);

}  // namespace core

#endif  // CORE_PAYLOAD_CORE_H
