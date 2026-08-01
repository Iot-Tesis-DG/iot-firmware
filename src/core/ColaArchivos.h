#ifndef CORE_COLA_ARCHIVOS_H
#define CORE_COLA_ARCHIVOS_H

#include <cstdint>
#include <string>
#include <vector>

namespace core {

/// Índice máximo representable con el formato "%05d.json".
static const int INDICE_MAXIMO = 99999;

/// Longitud máxima de un payload de lectura (RNF: 512 bytes, §3.5).
static const size_t PAYLOAD_MAX_BYTES = 512;

/**
 * Reglas del buffer FIFO en LittleFS, sin tocar el sistema de archivos.
 *
 * Lo que vive aquí es lo que decide si la cadena de evidencia de la tesis es
 * íntegra: qué nombre recibe cada lectura, en qué orden se drena y qué se hace
 * con un archivo que quedó a medio escribir tras un corte de corriente. Nada de
 * eso necesita flash para comprobarse, y sin extraerlo no había forma de
 * comprobarlo en absoluto.
 */

/// Nombre de archivo para un índice: "00001.json".
std::string nombreArchivo(int indice);

/// Índice codificado en un nombre de archivo. -1 si el nombre no tiene la
/// forma "<dígitos>.json".
///
/// Se parsea hasta el punto y no con un `substring(0, 5)` fijo: ese corte
/// leía "10000" de un hipotético "100000.json" y devolvía un índice ya usado.
int indiceDeNombre(const std::string& nombre);

/// Índice siguiente al último usado, con vuelta a 1 al agotar el formato de 5
/// dígitos.
///
/// Con la cadencia de 30 s el contador llega a 99999 en ~34 días de operación
/// continua. Sin vuelta explícita, `snprintf("%05d")` empezaba a emitir nombres
/// de 6 dígitos que rompían tanto el orden alfabético del drenaje como el
/// parseo del índice; la cola dejaba de ser FIFO justo en el escenario de
/// operación prolongada que la tesis quiere demostrar.
int siguienteIndice(int ultimoUsado);

/// Ordena los nombres por índice numérico ascendente (orden cronológico de
/// captura). Los nombres no reconocibles quedan al final, para que se drenen
/// —y se descarten— sin bloquear a los válidos.
void ordenarFIFO(std::vector<std::string>& nombres);

/**
 * ¿El contenido leído de flash es un payload completo?
 *
 * Un corte de corriente a mitad de `f.print()` deja un archivo truncado. Antes
 * solo se comprobaba que no estuviera vacío, así que medio JSON se publicaba
 * igual: el broker lo aceptaba, el backend lo rechazaba con `ValidationError`
 * y el archivo se borraba de todos modos. El resultado era una lectura perdida
 * en silencio y ruido en los logs del backend.
 *
 * La comprobación es deliberadamente sintáctica —llaves balanceadas fuera de
 * cadenas, tamaño dentro del límite—: no valida el esquema, que es trabajo del
 * backend, sino que descarta lo que sabemos que está roto antes de gastar
 * ancho de banda y una ranura de la cola en ello.
 */
bool esPayloadIntegro(const std::string& contenido);

}  // namespace core

#endif  // CORE_COLA_ARCHIVOS_H
