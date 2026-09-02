#include "PayloadCore.h"

#include <cstdio>

namespace core {

std::string escaparJSON(const std::string& entrada) {
    std::string salida;
    salida.reserve(entrada.size() + 8);
    for (char c : entrada) {
        switch (c) {
            case '"':  salida += "\\\""; break;
            case '\\': salida += "\\\\"; break;
            case '\n': salida += "\\n";  break;
            case '\r': salida += "\\r";  break;
            case '\t': salida += "\\t";  break;
            default:
                if ((unsigned char)c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
                    salida += buf;
                } else {
                    salida += c;
                }
        }
    }
    return salida;
}

/// Número con dos decimales, o `null` si el valor no es finito.
///
/// Dos decimales cubren de sobra la resolución de ambos sensores (±0.2 °C del
/// SHT31, 0.0625 °C del DS18B20) y hacen el tamaño del payload predecible, que
/// es lo que permite garantizar el techo de 512 bytes sin medirlo a posteriori.
static std::string numeroONulo(float valor) {
    if (std::isnan(valor) || std::isinf(valor)) return "null";
    char buf[24];
    snprintf(buf, sizeof(buf), "%.2f", (double)valor);
    return std::string(buf);
}

std::string serializarLectura(const Lectura& lectura, size_t maxBytes) {
    std::string json;
    json.reserve(420);

    // El orden de los campos replica el documentado en §3.5. JSON no le da
    // significado, pero mantenerlo hace que los payloads capturados en el
    // monitor serie se puedan comparar literalmente con la documentación.
    json += "{\"device_id\":\"";
    json += escaparJSON(lectura.deviceId);
    json += "\",\"timestamp\":\"";
    json += escaparJSON(lectura.timestamp);
    // HU-05: versión de esquema y reading_id explícitos. reading_id reutiliza
    // device_id+timestamp: ya es la clave de deduplicación del backend, así
    // que no hace falta un contador ni RTC con persistencia propia.
    json += "\",\"schema_version\":";
    {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", SCHEMA_VERSION_PAYLOAD);
        json += buf;
    }
    json += ",\"reading_id\":\"";
    json += escaparJSON(lectura.readingId);
    json += "\",\"estado_conectividad\":\"";
    json += (lectura.online ? "online" : "offline");
    json += "\",\"firmware_version\":\"";
    json += escaparJSON(lectura.firmwareVersion);
    json += "\",\"temperatura_interna\":";
    json += numeroONulo(lectura.temperaturaInterna);
    json += ",\"estado_temperatura_interna\":\"";
    json += nombreEstadoSensor(lectura.estadoTemperaturaInterna);
    json += "\",\"temperatura_ambiental\":";
    json += numeroONulo(lectura.temperaturaAmbiental);
    json += ",\"estado_temperatura_ambiental\":\"";
    json += nombreEstadoSensor(lectura.estadoTemperaturaAmbiental);
    json += "\",\"humedad_ambiental\":";
    json += numeroONulo(lectura.humedadAmbiental);
    json += ",\"estado_humedad_ambiental\":\"";
    json += nombreEstadoSensor(lectura.estadoHumedadAmbiental);
    json += "\",";
    // HU-04: sin MC-38 instalado, apertura_refrigerador viaja como `null` y
    // se añade `mc38_status`. El backend (payload_schema.py) todavía no
    // declara este campo — se agrega ahí en el mismo cambio que active esta
    // rama en el firmware de producción; mientras tanto los nodos con MC-38
    // instalado (el caso por defecto) no emiten este campo y no cambian.
    if (!lectura.mc38Instalado) {
        json += "\"apertura_refrigerador\":null,\"mc38_status\":\"not_installed\",";
    } else {
        json += "\"apertura_refrigerador\":";
        json += (lectura.aperturaRefrigerador ? "true" : "false");
        json += ",\"mc38_status\":\"ok\",";
    }
    json += "\"duracion_apertura_segundos\":";
    {
        char buf[16];
        snprintf(buf, sizeof(buf), "%lu", (unsigned long)lectura.duracionAperturaSegundos);
        json += buf;
    }
    json += "}";

    if (json.size() > maxBytes) return std::string();
    return json;
}

std::string extraerCampoString(const std::string& json, const std::string& campo) {
    const std::string clave = "\"" + campo + "\":\"";
    const size_t pos = json.find(clave);
    if (pos == std::string::npos) return std::string();
    const size_t inicio = pos + clave.size();
    const size_t fin = json.find('"', inicio);
    if (fin == std::string::npos) return std::string();
    return json.substr(inicio, fin - inicio);
}

}  // namespace core
