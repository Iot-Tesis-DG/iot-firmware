#ifndef CORE_CREDENCIALES_H
#define CORE_CREDENCIALES_H

#include <string>

namespace core {

/**
 * Reglas de aprovisionamiento (RNF-05), sin dependencias del SDK.
 *
 * El caso que esto evita es concreto: un nodo flasheado sin aprovisionar
 * arrancaba, intentaba conectar con los marcadores compilados y fallaba con un
 * `MQTT_CONNECT_BAD_CREDENTIALS` genérico, indistinguible de un token caducado
 * o de una regla ACL mal puesta. Diagnosticarlo requería mirar el binario.
 * Ahora el nodo detecta que no está aprovisionado y lo dice.
 */

/// Marcadores que nunca deben llegar a producción. Coinciden con los valores
/// por defecto que este repositorio tuvo compilados dentro del binario.
bool esMarcadorDeDesarrollo(const std::string& valor);

/// ¿El valor sirve como credencial? Vacío, solo espacios o marcador → no.
bool credencialValida(const std::string& valor);

/// ¿El conjunto permite intentar la conexión? Si no, el nodo debe seguir
/// capturando en modo offline en vez de entrar en un ciclo de reintentos.
bool aprovisionamientoCompleto(const std::string& ssid,
                               const std::string& mqttHost,
                               const std::string& mqttToken);

}  // namespace core

#endif  // CORE_CREDENCIALES_H
