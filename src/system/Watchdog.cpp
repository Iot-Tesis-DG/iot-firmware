#include "Watchdog.h"

#include <esp_idf_version.h>
#include <esp_task_wdt.h>

#include "../config.h"

// La API del TWDT cambió entre ESP-IDF 4.x (Arduino core 2.x) e IDF 5.x
// (Arduino core 3.x). Aquí SOLO está implementada la rama de IDF 4, que es la
// que fija `platform = espressif32@7.0.1` en platformio.ini y la única que se
// compila y se verifica en el CI.
//
// La rama de IDF 5 existió escrita pero nunca compilada, así que se retiró:
// código muerto no verificado en un firmware es peor que no tenerlo, porque
// aparenta soportar algo que nadie ha comprobado. Este `#error` convierte una
// futura actualización del core en un fallo de compilación explícito, con la
// instrucción de qué hay que reescribir, en vez de un cuelgue silencioso.
#if ESP_IDF_VERSION_MAJOR >= 5
#error "TWDT: esta versión de ESP-IDF usa esp_task_wdt_init(const esp_task_wdt_config_t*) \
y esp_task_wdt_reconfigure(). Reescribir inicializarWatchdog() y VERIFICARLO compilando \
antes de subir la versión de platform en platformio.ini."
#endif

void inicializarWatchdog() {
    // IDF 4.x: esp_err_t esp_task_wdt_init(uint32_t timeout_s, bool panic).
    // Llamarlo de nuevo cuando Arduino ya lo inicializó para `loopTask`
    // reconfigura el plazo, que es justo lo que queremos.
    const esp_err_t err = esp_task_wdt_init(WATCHDOG_TIMEOUT_S, /*panic=*/true);
    if (err != ESP_OK) {
        LOG_E("WDT", "No se pudo configurar el watchdog (err=%d).", (int)err);
        return;
    }
    LOG_I("WDT", "Task Watchdog a %u s (peor bloqueo no alimentable: %u ms x2).",
          WATCHDOG_TIMEOUT_S, PEOR_BLOQUEO_NO_ALIMENTABLE_MS);
}

void suscribirTareaAlWatchdog(const char* nombreTarea) {
    const esp_err_t err = esp_task_wdt_add(nullptr);  // nullptr = tarea actual
    if (err != ESP_OK) {
        LOG_E("WDT", "No se pudo suscribir '%s' al watchdog (err=%d).",
              nombreTarea, (int)err);
        return;
    }
    LOG_I("WDT", "Tarea '%s' suscrita al watchdog.", nombreTarea);
}

void alimentarWatchdog() {
    // Devuelve ESP_ERR_NOT_FOUND si la tarea no está suscrita. Se ignora a
    // propósito: así los drivers pueden llamarlo sin saber desde qué tarea
    // corren (p. ej. el sondeo del DS18B20 durante el arranque, en `setup()`).
    esp_task_wdt_reset();
}
