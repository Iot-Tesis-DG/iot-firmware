#include "ColaFIFO.h"

#include "ColaArchivos.h"

namespace core {

ColaFIFO::ColaFIFO(AlmacenLecturas& almacen, size_t maxArchivos, size_t maxBytesPorArchivo)
    : _almacen(almacen), _maxArchivos(maxArchivos), _maxBytes(maxBytesPorArchivo) {}

void ColaFIFO::sembrarIndice() {
    int mayor = 0;
    for (const auto& nombre : _almacen.listar()) {
        const int idx = indiceDeNombre(nombre);
        if (idx > mayor) mayor = idx;
    }
    _ultimoIndice = mayor;
}

std::vector<std::string> ColaFIFO::pendientes() {
    std::vector<std::string> nombres = _almacen.listar();
    ordenarFIFO(nombres);
    return nombres;
}

size_t ColaFIFO::cuenta() {
    return _almacen.listar().size();
}

bool ColaFIFO::guardar(const std::string& payload) {
    // Se valida ANTES de tocar flash: escribir un payload fuera de límite gasta
    // un ciclo de escritura para producir algo que el backend rechazará.
    if (payload.empty() || payload.size() > _maxBytes) return false;

    // Saturación: se libera el más antiguo. Se usa `while` y no `if` porque una
    // reducción de `_maxArchivos` entre versiones de firmware puede dejar la
    // cola por encima del límite y un solo borrado no la devolvería al rango.
    std::vector<std::string> actuales = pendientes();
    while (actuales.size() >= _maxArchivos && !actuales.empty()) {
        _almacen.borrar(actuales.front());
        actuales.erase(actuales.begin());
    }

    _ultimoIndice = siguienteIndice(_ultimoIndice);
    const std::string nombre = nombreArchivo(_ultimoIndice);

    // El índice solo puede repetirse si dio la vuelta tras 99999 lecturas. En
    // ese caso el archivo previo es, por construcción, el más antiguo.
    if (_almacen.existe(nombre)) _almacen.borrar(nombre);

    const size_t escritos = _almacen.escribir(nombre, payload);
    if (escritos != payload.size()) {
        // Escritura parcial: flash llena o partición dañada. Un archivo
        // truncado en la cola es peor que ninguno, porque se publicaría como
        // lectura válida y el backend lo rechazaría tras haberlo borrado.
        _almacen.borrar(nombre);
        return false;
    }
    return true;
}

bool ColaFIFO::leerIntegro(const std::string& nombre, std::string& salida) {
    std::string contenido;
    if (!_almacen.leer(nombre, contenido)) return false;
    if (contenido.size() > _maxBytes) return false;
    if (!esPayloadIntegro(contenido)) return false;
    salida = contenido;
    return true;
}

bool ColaFIFO::eliminar(const std::string& nombre) {
    return _almacen.borrar(nombre);
}

ResumenDrenaje drenar(ColaFIFO& cola, Publicador& publicador, int maxPorCiclo) {
    ResumenDrenaje resumen;

    const std::vector<std::string> lista = cola.pendientes();
    for (const auto& nombre : lista) {
        if (resumen.confirmados >= maxPorCiclo) break;

        std::string payload;
        if (!cola.leerIntegro(nombre, payload)) {
            // Ilegible o truncado por un corte de corriente a mitad de
            // escritura. No puede reintentarse eternamente ni debe frenar a las
            // lecturas posteriores, que sí son válidas.
            cola.eliminar(nombre);
            resumen.descartados++;
            continue;
        }

        const ResultadoPublicacion r = publicador.publicar(payload);
        if (r != ResultadoPublicacion::Confirmado) {
            // Sin PUBACK no se borra nada. El archivo se conserva y se
            // reintenta en el siguiente ciclo; el backend deduplica por
            // UNIQUE(device_id, timestamp), así que reenviar es inofensivo.
            resumen.detenido = true;
            break;
        }

        cola.eliminar(nombre);
        resumen.confirmados++;
    }
    return resumen;
}

}  // namespace core
