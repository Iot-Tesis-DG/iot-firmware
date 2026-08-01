#ifndef CORE_COLA_FIFO_H
#define CORE_COLA_FIFO_H

#include <cstddef>
#include <string>
#include <vector>

namespace core {

/**
 * Protocolo del buffer offline, separado del sistema de archivos.
 *
 * POR QUÉ EXISTE ESTA ABSTRACCIÓN
 * ------------------------------
 * La carrera real entre los dos núcleos del ESP32 no se puede reproducir en el
 * host: no hay dos schedulers. Lo que sí se puede reproducir es el **orden de
 * operaciones** que ambos núcleos ejecutan sobre la cola, intercalado a mano en
 * un solo hilo. Eso no demuestra que el planificador de FreeRTOS haga lo que
 * esperamos, pero sí demuestra que el algoritmo es correcto bajo cualquier
 * intercalado que el planificador pueda producir — que es la mitad del problema
 * y la única mitad verificable sin hardware.
 *
 * `LittleFSBuffer` implementa `AlmacenLecturas` sobre LittleFS y añade el mutex;
 * las pruebas lo implementan sobre un mapa en memoria capaz de simular
 * escrituras parciales y archivos truncados.
 */

/// Almacén de clave→contenido. La implementación real es LittleFS.
class AlmacenLecturas {
public:
    virtual ~AlmacenLecturas() = default;

    /// Nombres presentes, en cualquier orden.
    virtual std::vector<std::string> listar() = 0;
    virtual bool existe(const std::string& nombre) = 0;

    /// Escribe y devuelve los bytes efectivamente escritos. Un valor distinto
    /// de `contenido.size()` es una escritura parcial (flash llena o dañada).
    virtual size_t escribir(const std::string& nombre, const std::string& contenido) = 0;

    /// Lee el contenido. Devuelve false si el archivo no se puede abrir.
    virtual bool leer(const std::string& nombre, std::string& salida) = 0;

    virtual bool borrar(const std::string& nombre) = 0;
};

/**
 * Cola FIFO de lecturas pendientes con política de saturación e integridad.
 *
 * El índice se mantiene en RAM y es **monótono mientras el nodo siga
 * encendido**, incluso si la cola se vacía entre medias. Recalcularlo listando
 * el directorio (último+1) reintroducía un riesgo ABA: `drenarBuffer()` toma
 * una instantánea de nombres y los va procesando; si la cola se vacía y el
 * contador reinicia a 1, un nombre de esa instantánea puede referirse ya a una
 * lectura distinta de la que se publicó, y el borrado se lleva por delante un
 * dato nunca enviado. Con el contador monótono ese nombre no puede reaparecer.
 */
class ColaFIFO {
public:
    ColaFIFO(AlmacenLecturas& almacen, size_t maxArchivos, size_t maxBytesPorArchivo);

    /// Siembra el contador desde el almacén. Llamar al montar, para que un
    /// reinicio no reutilice nombres de lecturas aún pendientes de publicar.
    void sembrarIndice();

    /// Guarda un payload. Aplica FIFO si la cola está llena. Devuelve false si
    /// el payload está fuera de rango o la escritura fue parcial.
    bool guardar(const std::string& payload);

    /// Pendientes en orden cronológico.
    std::vector<std::string> pendientes();

    /// Lee un pendiente comprobando su integridad. Devuelve false si no se
    /// puede leer o si el contenido está truncado: en ambos casos el llamador
    /// debe descartarlo, no reintentarlo eternamente.
    bool leerIntegro(const std::string& nombre, std::string& salida);

    bool eliminar(const std::string& nombre);
    size_t cuenta();

    /// Último índice entregado. Expuesto para pruebas y diagnóstico.
    int ultimoIndice() const { return _ultimoIndice; }

private:
    AlmacenLecturas& _almacen;
    size_t _maxArchivos;
    size_t _maxBytes;
    int _ultimoIndice = 0;
};

// ---------------------------------------------------------------------------
// Drenaje hacia el broker
// ---------------------------------------------------------------------------

/// Resultado de publicar una lectura.
///
/// `Confirmado` significa PUBACK recibido del broker (QoS 1), no "escrito en el
/// socket". Es la única condición bajo la que se puede borrar la única copia
/// que existe de esa lectura.
enum class ResultadoPublicacion {
    Confirmado,
    Fallo,        ///< el broker no confirmó: conservar y reintentar
    SinConexion,  ///< no hay sesión: detener el drenaje
};

class Publicador {
public:
    virtual ~Publicador() = default;
    virtual ResultadoPublicacion publicar(const std::string& payload) = 0;
};

struct ResumenDrenaje {
    int confirmados = 0;
    int descartados = 0;   ///< archivos corruptos eliminados sin publicar
    bool detenido = false; ///< se cortó por fallo o pérdida de sesión
};

/**
 * Drena la cola en orden FIFO.
 *
 * Reglas, en orden de prioridad:
 *   1. Un archivo solo se borra tras `Confirmado`. Nunca antes.
 *   2. Un archivo corrupto se descarta y NO detiene la cola: reintentarlo
 *      eternamente bloquearía todas las lecturas posteriores.
 *   3. Al primer fallo de publicación se detiene: el orden cronológico de la
 *      cadena de evidencia importa más que el rendimiento.
 *   4. Como mucho `maxPorCiclo` publicaciones, para ceder CPU y no agotar el
 *      plazo del watchdog con la cola llena.
 */
ResumenDrenaje drenar(ColaFIFO& cola, Publicador& publicador, int maxPorCiclo);

}  // namespace core

#endif  // CORE_COLA_FIFO_H
