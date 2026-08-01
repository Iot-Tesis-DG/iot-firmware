#ifndef SYSTEM_WATCHDOG_H
#define SYSTEM_WATCHDOG_H

#include "../config.h"

/**
 * Task Watchdog Timer (TWDT) del ESP32.
 *
 * Antes de esto, el TWDT solo vigilaba `loopTask`, la tarea de Arduino, que en
 * este firmware no hace nada salvo `delay(1000)`. Es decir: se alimentaba
 * siempre y por tanto nunca disparaba. Las dos tareas que realmente importan
 * —`taskSensores` y `taskRed`— no estaban suscritas, así que si `taskRed` se
 * quedaba colgada dentro del handshake TLS o `taskSensores` en el bus 1-Wire,
 * el nodo se quedaba mudo indefinidamente y el único síntoma aguas abajo era el
 * LWT del broker 60 s después. En un refrigerador de farmacia sin nadie
 * mirando, eso son horas o días sin registro.
 *
 * PRESUPUESTO DEL PLAZO — de dónde sale el número
 * -----------------------------------------------
 * El plazo NO es un valor redondo elegido a ojo. Se deriva del mayor tramo de
 * código que puede ejecutarse SIN poder alimentar al watchdog, es decir, del
 * bloqueo más largo que ocurre dentro de una sola llamada a una API del SDK
 * que no devuelve el control hasta terminar.
 *
 * Inventario de bloqueos de `taskRed` (el peor de los dos núcleos):
 *
 *   | Tramo                                    | Cota      | ¿Alimentable?  |
 *   |------------------------------------------|-----------|----------------|
 *   | Asociación Wi-Fi (`WiFi.begin` + espera) | 15 000 ms | SÍ, cada 500 ms|
 *   | Handshake TLS (`WiFiClientSecure`)       | 10 000 ms | NO             |
 *   | CONNACK del broker (command timeout)     |  5 000 ms | NO             |
 *   | PUBACK por lectura (command timeout)     |  5 000 ms | NO             |
 *   | `getLocalTime()` de NTP                  | 10 000 ms | NO             |
 *   | `mqtt.loop()`                            |  < 100 ms | —              |
 *
 * El peor tramo NO alimentable es `mqtt.connect()`: el handshake TLS y la
 * espera del CONNACK ocurren dentro de la misma llamada, sin punto intermedio
 * donde intercalar un `esp_task_wdt_reset()`. Son 10 000 + 5 000 = 15 000 ms.
 * NTP (10 s) y cada PUBACK (5 s) quedan por debajo, y entre ellos sí se
 * alimenta (ver `taskRed` en main.cpp).
 *
 * En `taskSensores` el peor tramo no alimentable es una transacción I2C del
 * SHT31 (~15 ms); el sondeo del DS18B20 llega a 2 000 ms pero alimenta cada
 * 10 ms, y la espera de la ventana de 30 s alimenta cada 50 ms. La espera por
 * el mutex del buffer está acotada por una operación de flash (decenas de ms),
 * porque el mutex nunca se mantiene durante una publicación MQTT.
 *
 * Plazo = 2 × 15 000 ms = 30 s. El factor 2 cubre la variabilidad de
 * planificación entre dos núcleos con Wi-Fi activo, no un tramo adicional.
 *
 * El cálculo es automático: si alguien sube `TLS_HANDSHAKE_TIMEOUT_MS` o
 * `MQTT_COMMAND_TIMEOUT_MS`, el plazo del watchdog sube con ellos, y el
 * `static_assert` impide que el margen se quede corto en silencio.
 */

/// Mayor tramo que puede ejecutarse sin alimentar el watchdog (ver tabla).
static const unsigned int PEOR_BLOQUEO_NO_ALIMENTABLE_MS =
    TLS_HANDSHAKE_TIMEOUT_MS + MQTT_COMMAND_TIMEOUT_MS;

/// Plazo del TWDT en segundos: el peor bloqueo con un factor 2 de margen.
static const unsigned int WATCHDOG_TIMEOUT_S =
    (2 * PEOR_BLOQUEO_NO_ALIMENTABLE_MS + 999) / 1000;

// Estas dos comprobaciones SÍ pueden fallar, y ese es el objetivo: fijan las
// hipótesis del análisis de arriba para que un cambio de constantes no las
// invalide en silencio.
//
// (1) `mqtt.connect()` debe seguir siendo el peor bloqueo no alimentable. Si
//     alguien sube el timeout de NTP por encima de él, el análisis deja de ser
//     válido y hay que rehacer la tabla.
static_assert(PEOR_BLOQUEO_NO_ALIMENTABLE_MS >= NTP_SYNC_TIMEOUT_MS,
              "NTP pasó a ser el bloqueo no alimentable más largo: rehacer el "
              "presupuesto del watchdog en system/Watchdog.h.");

// (2) El plazo del watchdog debe quedar por debajo del keep-alive MQTT. Si lo
//     superara, EMQX daría el nodo por muerto y publicaría su LWT ANTES de que
//     el watchdog llegara a reiniciarlo: el nodo aparecería como caído sin
//     recuperarse, que es el peor de los dos comportamientos.
static_assert(WATCHDOG_TIMEOUT_S <= MQTT_KEEPALIVE_SEC,
              "El plazo del watchdog supera el keep-alive MQTT: el broker "
              "declararía el nodo muerto antes de que el watchdog lo reinicie.");

/// Reconfigura el TWDT al plazo del proyecto. Llamar una vez en `setup()`.
void inicializarWatchdog();

/// Suscribe la tarea actual al TWDT. Llamar al entrar en cada tarea.
void suscribirTareaAlWatchdog(const char* nombreTarea);

/// Alimenta el watchdog desde la tarea actual. Seguro de llamar desde tareas
/// no suscritas: en ese caso no hace nada.
void alimentarWatchdog();

#endif  // SYSTEM_WATCHDOG_H
