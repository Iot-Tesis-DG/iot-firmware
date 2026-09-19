#ifndef SYSTEM_BOOT_ID_H
#define SYSTEM_BOOT_ID_H

#include <cstdint>

/**
 * HU-01/HU-11 (backlog 54 HU): boot_id identifica de forma persistente cada
 * arranque del nodo, para que device_id+boot_id+seq_no sea una clave de
 * idempotencia real incluso si el nodo pierde alimentación a mitad de un
 * ciclo de publicación (a diferencia de reading_id=device_id+timestamp, que
 * un reloj sin sincronizar podía repetir).
 *
 * Se guarda en el mismo espacio de nombres NVS que las credenciales
 * (`CREDENCIALES_NVS_NS`), en una clave propia — a diferencia de
 * `cargarCredenciales()` (solo lectura, ver system/Credenciales.h), esta
 * función SÍ escribe: es la única responsabilidad de todo el firmware que
 * requiere persistir un contador entre arranques.
 */

/// Lee el boot_id actual de NVS, lo incrementa, lo persiste y devuelve el
/// nuevo valor. Primer arranque tras flashear (NVS vacío): empieza en 1, no
/// en 0 — 0 queda reservado para "boot_id no asignado" en cualquier
/// diagnóstico que necesite distinguir ambos casos.
uint32_t cargarEIncrementarBootId();

#endif  // SYSTEM_BOOT_ID_H
