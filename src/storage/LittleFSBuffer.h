#ifndef LITTLEFS_BUFFER_H
#define LITTLEFS_BUFFER_H

#include <Arduino.h>
#include <LittleFS.h>
#include <vector>

/**
 * Buffer offline-first sobre LittleFS.
 *
 * Estructura:
 *   /littlefs/pending/
 *       00001.json   ← lectura más antigua (FIFO)
 *       00002.json
 *       ...
 *       NNNNN.json   ← lectura más reciente
 *
 * Cada archivo contiene un payload JSON de ~250 bytes.
 *
 * Política FIFO: cuando se alcanza LITTLEFS_MAX_FILES, se elimina el más
 * antiguo y se registra evento de saturación.
 *
 * Los archivos se eliminan tras publicarlos y comprobar que la sesión MQTT
 * sigue viva (la publicación es QoS 0: no hay PUBACK que esperar).
 * En la práctica, la sincronización se hace por lote: cuando el ESP32 se
 * reconecta, envía todos los pendientes en orden cronológico, y solo al
 * completar la cola entera se vacía el directorio.
 */
class LittleFSBuffer {
public:
    bool begin();
    bool saveReading(const char* jsonPayload);
    bool hasPending() const;
    int pendingCount() const;

    /// Devuelve la lista de archivos pendientes ordenados por nombre
    /// (que es el orden cronológico de captura).
    std::vector<String> listPendingFiles() const;

    /// Lee el contenido de un archivo pendiente.
    String readFile(const String& filename) const;

    /// Elimina un archivo ya publicado.
    bool removeFile(const String& filename);

    /// Elimina TODOS los archivos pendientes (tras sincronización completa).
    void clearAll();

    /// Espacio libre en LittleFS (bytes).
    size_t freeSpace() const;

    /// Espacio usado (bytes).
    size_t usedSpace() const;

private:
    String _pendingDir = "/pending";

    String _makeFilename(int index) const;
    int _nextFileIndex() const;
};

#endif // LITTLEFS_BUFFER_H
