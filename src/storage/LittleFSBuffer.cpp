#include "LittleFSBuffer.h"
#include "../config.h"
#include <SPIFFS.h>  // Necesario para compatibilidad de LittleFS con Arduino


bool LittleFSBuffer::begin() {
    if (!LittleFS.begin(true, LITTLEFS_MOUNT_POINT, 10, "littlefs")) {
        LOG_E("LittleFS", "Fallo al montar LittleFS en '%s'.", LITTLEFS_MOUNT_POINT);
        return false;
    }

    // Crear directorio de pendientes si no existe
    if (!LittleFS.exists(_pendingDir)) {
        if (!LittleFS.mkdir(_pendingDir)) {
            LOG_E("LittleFS", "No se pudo crear '%s'.", _pendingDir.c_str());
            return false;
        }
    }

    int pendientes = pendingCount();
    LOG_I("LittleFS", "Montado. %d lecturas pendientes de envío. %d KB libres.",
          pendientes, freeSpace() / 1024);
    return true;
}

bool LittleFSBuffer::saveReading(const char* jsonPayload) {
    if (pendingCount() >= LITTLEFS_MAX_FILES) {
        // FIFO: eliminar el más antiguo para hacer espacio
        auto files = listPendingFiles();
        if (!files.empty()) {
            LOG_I("LittleFS", "Buffer lleno (%d/%d). Aplicando FIFO: descartando %s.",
                  LITTLEFS_MAX_FILES, LITTLEFS_MAX_FILES, files[0].c_str());
            removeFile(files[0]);
        }
    }

    int nextIndex = _nextFileIndex();
    String filename = _makeFilename(nextIndex);
    String fullPath = _pendingDir + "/" + filename;

    File f = LittleFS.open(fullPath, "w");
    if (!f) {
        LOG_E("LittleFS", "No se pudo crear '%s'.", fullPath.c_str());
        return false;
    }

    size_t written = f.print(jsonPayload);
    f.close();

    if (written == 0) {
        LOG_E("LittleFS", "Escritura vacía en '%s'.", fullPath.c_str());
        LittleFS.remove(fullPath);
        return false;
    }

    LOG_I("LittleFS", "Guardado: %s (%d bytes, %d/%d pendientes).",
          filename.c_str(), written, pendingCount(), LITTLEFS_MAX_FILES);
    return true;
}

bool LittleFSBuffer::hasPending() const {
    return pendingCount() > 0;
}

int LittleFSBuffer::pendingCount() const {
    File dir = LittleFS.open(_pendingDir);
    if (!dir || !dir.isDirectory()) return 0;

    int count = 0;
    File f = dir.openNextFile();
    while (f) {
        if (!f.isDirectory()) count++;
        f = dir.openNextFile();
    }
    return count;
}

std::vector<String> LittleFSBuffer::listPendingFiles() const {
    std::vector<String> result;
    File dir = LittleFS.open(_pendingDir);
    if (!dir || !dir.isDirectory()) return result;

    File f = dir.openNextFile();
    while (f) {
        if (!f.isDirectory()) {
            result.push_back(String(f.name()));
        }
        f = dir.openNextFile();
    }

    // Ordenar por nombre (que es numérico secuencial → orden cronológico)
    std::sort(result.begin(), result.end());
    return result;
}

String LittleFSBuffer::readFile(const String& filename) const {
    String fullPath = _pendingDir + "/" + filename;
    File f = LittleFS.open(fullPath, "r");
    if (!f) {
        LOG_E("LittleFS", "No se pudo leer '%s'.", fullPath.c_str());
        return "";
    }

    String content = f.readString();
    f.close();
    return content;
}

bool LittleFSBuffer::removeFile(const String& filename) {
    String fullPath = _pendingDir + "/" + filename;
    if (LittleFS.remove(fullPath)) {
        LOG_I("LittleFS", "Eliminado (publicado): %s", filename.c_str());
        return true;
    }
    LOG_E("LittleFS", "No se pudo eliminar '%s'.", fullPath.c_str());
    return false;
}

void LittleFSBuffer::clearAll() {
    auto files = listPendingFiles();
    for (const auto& f : files) {
        removeFile(f);
    }
    LOG_I("LittleFS", "Buffer vaciado (%d archivos eliminados).", files.size());
}

size_t LittleFSBuffer::freeSpace() const {
    return LittleFS.totalBytes() - LittleFS.usedBytes();
}

size_t LittleFSBuffer::usedSpace() const {
    return LittleFS.usedBytes();
}

String LittleFSBuffer::_makeFilename(int index) const {
    // 16 bytes, no 10: "00001.json" son 10 caracteres MÁS el terminador, así
    // que con `char buf[10]` snprintf truncaba y todos los archivos se creaban
    // como "00001.jso". El buffer funcionaba —se listaban y borraban con el
    // mismo nombre truncado— pero no coincidía con lo documentado y confundía
    // cualquier inspección del sistema de archivos.
    char buf[16];
    snprintf(buf, sizeof(buf), "%05d.json", index);
    return String(buf);
}

int LittleFSBuffer::_nextFileIndex() const {
    auto files = listPendingFiles();
    if (files.empty()) return 1;

    // El último archivo (ordenado alfabéticamente) tiene el índice más alto.
    String last = files.back();
    // Extraer número de "NNNNN.json"
    int idx = last.substring(0, 5).toInt();
    return idx + 1;
}
