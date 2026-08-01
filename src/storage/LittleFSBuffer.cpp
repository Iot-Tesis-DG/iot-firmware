#include "LittleFSBuffer.h"

#include "../config.h"

// Nota: ya no se incluye <SPIFFS.h>. No aportaba nada —LittleFS no depende de
// él— y arrastraba al binario el driver de un sistema de archivos que este
// firmware no monta.

const char* LittleFSBuffer::AlmacenLittleFS::DIRECTORIO = "/pending";

// =========================================================================
// Adaptador sobre LittleFS
// =========================================================================

String LittleFSBuffer::AlmacenLittleFS::rutaCompleta(const std::string& nombre) const {
    return String(DIRECTORIO) + "/" + String(nombre.c_str());
}

std::vector<std::string> LittleFSBuffer::AlmacenLittleFS::listar() {
    std::vector<std::string> nombres;

    File dir = LittleFS.open(DIRECTORIO);
    if (!dir || !dir.isDirectory()) return nombres;

    File f = dir.openNextFile();
    while (f) {
        if (!f.isDirectory()) nombres.push_back(std::string(f.name()));
        // Los descriptores se cierran explícitamente: `openNextFile()` deja el
        // anterior abierto y con 200 archivos se agotaba el límite de la VFS.
        f.close();
        f = dir.openNextFile();
    }
    dir.close();
    return nombres;
}

bool LittleFSBuffer::AlmacenLittleFS::existe(const std::string& nombre) {
    return LittleFS.exists(rutaCompleta(nombre));
}

size_t LittleFSBuffer::AlmacenLittleFS::escribir(const std::string& nombre,
                                                 const std::string& contenido) {
    const String ruta = rutaCompleta(nombre);
    File f = LittleFS.open(ruta, "w");
    if (!f) {
        LOG_E("LittleFS", "No se pudo crear '%s'.", ruta.c_str());
        return 0;
    }
    const size_t escritos = f.print(contenido.c_str());
    f.close();
    return escritos;
}

bool LittleFSBuffer::AlmacenLittleFS::leer(const std::string& nombre, std::string& salida) {
    const String ruta = rutaCompleta(nombre);
    File f = LittleFS.open(ruta, "r");
    if (!f) return false;

    // Cota dura antes de cargar en RAM: un archivo corrupto con un tamaño
    // disparatado no puede reservar el heap del nodo.
    if (f.size() > (size_t)LITTLEFS_MAX_FILESIZE) {
        LOG_E("LittleFS", "'%s' mide %u bytes (máx %d).", ruta.c_str(),
              (unsigned)f.size(), LITTLEFS_MAX_FILESIZE);
        f.close();
        return false;
    }

    const String contenido = f.readString();
    f.close();
    salida = std::string(contenido.c_str());
    return true;
}

bool LittleFSBuffer::AlmacenLittleFS::borrar(const std::string& nombre) {
    return LittleFS.remove(rutaCompleta(nombre));
}

// =========================================================================
// LittleFSBuffer
// =========================================================================

LittleFSBuffer::LittleFSBuffer()
    : _cola(_almacen, LITTLEFS_MAX_FILES, LITTLEFS_MAX_FILESIZE) {
    _mux = xSemaphoreCreateRecursiveMutex();
}

LittleFSBuffer::~LittleFSBuffer() {
    if (_mux != nullptr) vSemaphoreDelete(_mux);
}

bool LittleFSBuffer::begin() {
    GuardaBuffer guarda(_mux);

    if (!LittleFS.begin(true, LITTLEFS_MOUNT_POINT, 10, "littlefs")) {
        LOG_E("LittleFS", "Fallo al montar LittleFS en '%s'.", LITTLEFS_MOUNT_POINT);
        return false;
    }

    if (!LittleFS.exists(AlmacenLittleFS::DIRECTORIO)) {
        if (!LittleFS.mkdir(AlmacenLittleFS::DIRECTORIO)) {
            LOG_E("LittleFS", "No se pudo crear '%s'.", AlmacenLittleFS::DIRECTORIO);
            return false;
        }
    }

    // El contador se siembra desde disco para que un reinicio no reutilice
    // nombres de lecturas que siguen pendientes de publicar.
    _cola.sembrarIndice();

    LOG_I("LittleFS", "Montado. %u pendientes (último índice %d). %u KB libres.",
          (unsigned)_cola.cuenta(), _cola.ultimoIndice(), (unsigned)(freeSpace() / 1024));
    return true;
}

bool LittleFSBuffer::saveReading(const char* jsonPayload) {
    if (jsonPayload == nullptr) return false;

    GuardaBuffer guarda(_mux);
    const bool ok = _cola.guardar(std::string(jsonPayload));
    if (!ok) {
        LOG_E("LittleFS", "No se pudo guardar la lectura (%u bytes).",
              (unsigned)strlen(jsonPayload));
    }
    return ok;
}

bool LittleFSBuffer::hasPending() {
    return pendingCount() > 0;
}

int LittleFSBuffer::pendingCount() {
    GuardaBuffer guarda(_mux);
    return (int)_cola.cuenta();
}

std::vector<String> LittleFSBuffer::listPendingFiles() {
    GuardaBuffer guarda(_mux);

    const std::vector<std::string> nombres = _cola.pendientes();
    std::vector<String> resultado;
    resultado.reserve(nombres.size());
    for (const auto& n : nombres) resultado.push_back(String(n.c_str()));
    return resultado;
}

String LittleFSBuffer::readFile(const String& filename) {
    GuardaBuffer guarda(_mux);

    std::string contenido;
    if (!_cola.leerIntegro(std::string(filename.c_str()), contenido)) {
        LOG_E("LittleFS", "'%s' ilegible o truncado — se descartará.", filename.c_str());
        return String();
    }
    return String(contenido.c_str());
}

bool LittleFSBuffer::removeFile(const String& filename) {
    GuardaBuffer guarda(_mux);

    if (_cola.eliminar(std::string(filename.c_str()))) {
        LOG_I("LittleFS", "Eliminado: %s", filename.c_str());
        return true;
    }
    LOG_E("LittleFS", "No se pudo eliminar '%s'.", filename.c_str());
    return false;
}

void LittleFSBuffer::clearAll() {
    GuardaBuffer guarda(_mux);

    const auto files = _cola.pendientes();
    for (const auto& f : files) _cola.eliminar(f);
    LOG_I("LittleFS", "Buffer vaciado (%u archivos).", (unsigned)files.size());
}

size_t LittleFSBuffer::freeSpace() {
    return LittleFS.totalBytes() - LittleFS.usedBytes();
}

size_t LittleFSBuffer::usedSpace() {
    return LittleFS.usedBytes();
}
