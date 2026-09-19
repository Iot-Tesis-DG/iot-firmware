#include "BootId.h"

#include <Preferences.h>

#include "../config.h"

uint32_t cargarEIncrementarBootId() {
    Preferences nvs;
    // Lectura-escritura (a diferencia de cargarCredenciales(), que abre en
    // modo solo-lectura): este es el único contador que el firmware persiste
    // entre arranques.
    if (!nvs.begin(CREDENCIALES_NVS_NS, false)) {
        // Espacio de nombres no disponible (NVS corrupta/sin formatear): no
        // se puede persistir. Se degrada a 0 — boot_id deja de ser útil para
        // idempotencia en ESTE arranque, pero no bloquea la captura ni la
        // publicación (mismo criterio que un dispositivo sin MC-38: un campo
        // opcional ausente no detiene el resto del pipeline).
        return 0;
    }
    const uint32_t anterior = nvs.getUInt("boot_id", 0);
    const uint32_t nuevo = anterior + 1;
    nvs.putUInt("boot_id", nuevo);
    nvs.end();
    return nuevo;
}
