#ifndef LITTLEFS_BUFFER_H
#define LITTLEFS_BUFFER_H

#include <Arduino.h>
#include <LittleFS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <vector>

#include "../core/ColaFIFO.h"

/**
 * Buffer offline-first sobre LittleFS.
 *
 * Estructura:
 *   /littlefs/pending/
 *       00001.json   ← lectura más antigua (FIFO)
 *       ...
 *       NNNNN.json   ← lectura más reciente
 *
 * Esta clase aporta dos cosas y solo dos: el acceso real a LittleFS
 * (`AlmacenLittleFS`) y la exclusión mutua entre núcleos. El protocolo de la
 * cola —índices, orden FIFO, saturación, integridad— vive en `core::ColaFIFO`,
 * que se prueba en el host con intercalados adversarios.
 *
 * ACCESO DESDE DOS NÚCLEOS
 * ------------------------
 * Es la única estructura que ambas tareas tocan a la vez: el Core 0 escribe una
 * lectura cada 30 s y el Core 1 recorre, lee y borra la cola. No había ninguna
 * sincronización, y las operaciones no son atómicas:
 *
 *   - El índice se calculaba listando el directorio y sumando 1. Si el Core 1
 *     borraba entre el listado y el `open()`, el índice se reutilizaba.
 *   - `openNextFile()` mantiene un cursor de directorio abierto mientras el
 *     otro núcleo crea o borra entradas en ese mismo directorio.
 *
 * El mutex NO se mantiene durante la publicación MQTT: `drenarBuffer()` lee
 * bajo mutex, publica fuera y borra bajo mutex otra vez. Así la espera máxima
 * del Core 0 es una operación de flash (decenas de ms) y no un PUBACK (hasta
 * MQTT_COMMAND_TIMEOUT_MS), que sí habría comprometido la cadencia de 30 s.
 */
class LittleFSBuffer {
public:
    LittleFSBuffer();
    ~LittleFSBuffer();

    bool begin();
    bool saveReading(const char* jsonPayload);
    bool hasPending();
    int pendingCount();

    /// Pendientes en orden cronológico.
    std::vector<String> listPendingFiles();

    /// Lee un pendiente. Devuelve "" si no se puede leer O si el contenido no
    /// es un payload íntegro (archivo truncado por un corte de corriente).
    String readFile(const String& filename);

    bool removeFile(const String& filename);
    void clearAll();

    size_t freeSpace();
    size_t usedSpace();

private:
    /// Adaptador de `core::AlmacenLecturas` sobre LittleFS.
    class AlmacenLittleFS : public core::AlmacenLecturas {
    public:
        std::vector<std::string> listar() override;
        bool existe(const std::string& nombre) override;
        size_t escribir(const std::string& nombre, const std::string& contenido) override;
        bool leer(const std::string& nombre, std::string& salida) override;
        bool borrar(const std::string& nombre) override;

        String rutaCompleta(const std::string& nombre) const;
        static const char* DIRECTORIO;
    };

    AlmacenLittleFS _almacen;
    core::ColaFIFO _cola;
    SemaphoreHandle_t _mux = nullptr;
};

/// RAII sobre el mutex del buffer: garantiza el `give` en todos los caminos de
/// salida, incluidos los `return` tempranos por error de flash.
class GuardaBuffer {
public:
    explicit GuardaBuffer(SemaphoreHandle_t mux) : _mux(mux) {
        if (_mux != nullptr) xSemaphoreTakeRecursive(_mux, portMAX_DELAY);
    }
    ~GuardaBuffer() {
        if (_mux != nullptr) xSemaphoreGiveRecursive(_mux);
    }
    GuardaBuffer(const GuardaBuffer&) = delete;
    GuardaBuffer& operator=(const GuardaBuffer&) = delete;

private:
    SemaphoreHandle_t _mux;
};

#endif  // LITTLEFS_BUFFER_H
