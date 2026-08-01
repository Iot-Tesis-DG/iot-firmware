#ifndef CORE_RANGOS_SENSORES_H
#define CORE_RANGOS_SENSORES_H

#include <cmath>

namespace core {

/**
 * Validación de rango de las lecturas de sensores.
 *
 * La regla es una sola y no admite excepciones: una lectura que no se puede
 * confiar sale como NaN, nunca como un número. El firmware ya lo hacía, pero
 * repartido en tres `if` distintos con constantes duplicadas en dos cabeceras,
 * y sin ninguna prueba que fijara el contrato. Aquí queda en un solo sitio y
 * cubierto, porque es el equivalente edge del defecto B-05 del backend: un 0.0
 * por una lectura fallida es indistinguible de 0.0 °C reales, que en una
 * cadena de frío de 2-8 °C es precisamente una excursión.
 *
 * Los rangos son los físicos de cada hoja de datos, no los operativos: filtrar
 * aquí a 2-8 °C escondería justamente las excursiones que hay que detectar.
 */

// DS18B20: -55..125 °C. -127 °C es el código de error de DallasTemperature
// (sensor ausente, bus en corto). 85 °C es el valor de power-on reset y es un
// valor físicamente posible, así que NO se filtra: se deja pasar y el backend
// decide. Filtrarlo aquí ocultaría un fallo de cableado real.
static const float DS18B20_ERROR      = -127.0f;
static const float DS18B20_MIN_VALIDO = -55.0f;
static const float DS18B20_MAX_VALIDO = 125.0f;

// SHT31-DIS: -40..125 °C y 0..100 %HR.
static const float SHT31_TEMP_MIN = -40.0f;
static const float SHT31_TEMP_MAX = 125.0f;
static const float SHT31_HUM_MIN  = 0.0f;
static const float SHT31_HUM_MAX  = 100.0f;

/// Devuelve `valor` si está dentro de [min, max] y es finito; NaN en otro caso.
inline float validarRango(float valor, float minimo, float maximo) {
    if (std::isnan(valor) || std::isinf(valor)) return NAN;
    if (valor < minimo || valor > maximo) return NAN;
    return valor;
}

/// Temperatura del DS18B20. Trata el código de error -127 °C como avería.
inline float validarDS18B20(float tempC) {
    if (std::isnan(tempC) || std::isinf(tempC)) return NAN;
    if (tempC <= DS18B20_ERROR) return NAN;
    return validarRango(tempC, DS18B20_MIN_VALIDO, DS18B20_MAX_VALIDO);
}

/// ¿La lectura cruda del DS18B20 indica sensor ausente o bus roto?
inline bool esFalloDS18B20(float tempC) {
    return std::isnan(tempC) || std::isinf(tempC) || tempC <= DS18B20_ERROR;
}

inline float validarSHT31Temperatura(float tempC) {
    return validarRango(tempC, SHT31_TEMP_MIN, SHT31_TEMP_MAX);
}

inline float validarSHT31Humedad(float humedadPct) {
    return validarRango(humedadPct, SHT31_HUM_MIN, SHT31_HUM_MAX);
}

}  // namespace core

#endif  // CORE_RANGOS_SENSORES_H
