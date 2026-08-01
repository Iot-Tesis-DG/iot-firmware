#include "ColaArchivos.h"

#include <algorithm>
#include <cstdio>

namespace core {

std::string nombreArchivo(int indice) {
    if (indice < 1) indice = 1;
    if (indice > INDICE_MAXIMO) indice = INDICE_MAXIMO;
    char buf[16];
    snprintf(buf, sizeof(buf), "%05d.json", indice);
    return std::string(buf);
}

int indiceDeNombre(const std::string& nombre) {
    const size_t punto = nombre.find('.');
    if (punto == std::string::npos || punto == 0) return -1;
    if (nombre.compare(punto, std::string::npos, ".json") != 0) return -1;

    long valor = 0;
    for (size_t i = 0; i < punto; ++i) {
        const char c = nombre[i];
        if (c < '0' || c > '9') return -1;
        valor = valor * 10 + (c - '0');
        if (valor > INDICE_MAXIMO) return -1;  // fuera del formato soportado
    }
    return (int)valor;
}

int siguienteIndice(int ultimoUsado) {
    if (ultimoUsado < 1) return 1;
    if (ultimoUsado >= INDICE_MAXIMO) return 1;  // vuelta al inicio
    return ultimoUsado + 1;
}

void ordenarFIFO(std::vector<std::string>& nombres) {
    std::sort(nombres.begin(), nombres.end(),
              [](const std::string& a, const std::string& b) {
                  const int ia = indiceDeNombre(a);
                  const int ib = indiceDeNombre(b);
                  if (ia < 0 && ib < 0) return a < b;
                  if (ia < 0) return false;  // inválidos al final
                  if (ib < 0) return true;
                  return ia < ib;
              });
}

bool esPayloadIntegro(const std::string& contenido) {
    if (contenido.empty() || contenido.size() > PAYLOAD_MAX_BYTES) return false;

    size_t ini = 0;
    size_t fin = contenido.size();
    while (ini < fin && (unsigned char)contenido[ini] <= ' ') ini++;
    while (fin > ini && (unsigned char)contenido[fin - 1] <= ' ') fin--;
    if (ini >= fin) return false;
    if (contenido[ini] != '{' || contenido[fin - 1] != '}') return false;

    int profundidad = 0;
    bool enCadena = false;
    bool escapado = false;
    for (size_t i = ini; i < fin; ++i) {
        const char c = contenido[i];
        if (enCadena) {
            if (escapado)          escapado = false;
            else if (c == '\\')    escapado = true;
            else if (c == '"')     enCadena = false;
            continue;
        }
        if (c == '"')      enCadena = true;
        else if (c == '{') profundidad++;
        else if (c == '}') {
            profundidad--;
            if (profundidad < 0) return false;
        }
    }
    return profundidad == 0 && !enCadena;
}

}  // namespace core
