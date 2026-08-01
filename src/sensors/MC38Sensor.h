#ifndef MC38_SENSOR_H
#define MC38_SENSOR_H

#include <Arduino.h>

#include "../core/Antirrebote.h"

/**
 * Sensor MC-38 — contacto magnético de puerta (reed switch).
 *
 * Conectado a un pin GPIO con pull-up interno.
 * LOW  = puerta cerrada (imán presente, contacto cerrado).
 * HIGH = puerta abierta (imán ausente, contacto abierto).
 *
 * MUESTREO Y REPORTE SON COSAS DISTINTAS
 * --------------------------------------
 * `poll()` debe llamarse con frecuencia (cada ~50 ms) para que el antirrebote
 * signifique algo y para no perder aperturas. El payload solo se construye
 * cada 30 s, y una puerta de farmacia se abre y se cierra en 10-20 s: si solo
 * se mirase el estado instantáneo en el momento del reporte, la mayoría de las
 * aperturas reales serían invisibles.
 *
 * Por eso se acumula entre reportes:
 *   - `huboApertura()`     → ¿se abrió en algún momento de la ventana?
 *   - `duracionAperturaSegundos()` → segundos abierta acumulados en la ventana
 *   - `limpiarReporte()`   → llamar tras publicar, para empezar otra ventana
 *
 * Esto es lo que exige RF-03 / HU-04: detectar la apertura, no muestrearla con
 * suerte.
 *
 * Esta clase solo hace de puente con el GPIO y el reloj: la máquina de estados
 * del antirrebote vive en `core::Antirrebote`, que se prueba en el host.
 */
class MC38Sensor {
public:
    explicit MC38Sensor(uint8_t pin);

    void begin();

    /// Muestrea el pin y aplica antirrebote. Llamar cada ~POLL_MS.
    void poll();

    /// Estado estable actual: true = abierta.
    bool isOpen() const;

    /// ¿La puerta estuvo abierta en algún instante desde `limpiarReporte()`?
    bool huboApertura() const;

    /// Segundos acumulados con la puerta abierta desde `limpiarReporte()`,
    /// incluida la apertura en curso si sigue abierta.
    unsigned long duracionAperturaSegundos() const;

    /// Cierra la ventana de reporte. Llamar justo después de construir el
    /// payload; si la puerta sigue abierta, la nueva ventana sigue contando.
    void limpiarReporte();

    /// Cadencia recomendada de `poll()`.
    static constexpr unsigned long POLL_MS = 50;
    static constexpr unsigned long DEBOUNCE_MS = 50;

private:
    uint8_t _pin;
    core::Antirrebote _antirrebote{DEBOUNCE_MS};
};

#endif  // MC38_SENSOR_H
