#include "Credenciales.h"

#include <Preferences.h>

#include "../config.h"
#include "../core/Credenciales.h"

bool CredencialesNodo::completas() const {
    return core::aprovisionamientoCompleto(std::string(wifiSsid.c_str()),
                                           std::string(mqttHost.c_str()),
                                           std::string(mqttToken.c_str()));
}

/// Devuelve el valor de NVS si es utilizable; si no, el de la build_flag.
static String _elegir(Preferences& nvs, const char* clave, const char* respaldo,
                      bool& vinoDeNVS) {
    const String enNVS = nvs.getString(clave, "");
    if (core::credencialValida(std::string(enNVS.c_str()))) {
        vinoDeNVS = true;
        return enNVS;
    }
    vinoDeNVS = false;
    return String(respaldo != nullptr ? respaldo : "");
}

CredencialesNodo cargarCredenciales() {
    CredencialesNodo cred;

    Preferences nvs;
    // Solo lectura: el firmware nunca escribe credenciales, así el binario no
    // puede filtrarlas ni corromper el aprovisionamiento.
    const bool hayNVS = nvs.begin(CREDENCIALES_NVS_NS, true);
    if (!hayNVS) {
        // Espacio de nombres inexistente = nodo sin aprovisionar por NVS. No es
        // un error: se cae a las build_flags.
        cred.wifiSsid = WIFI_SSID;
        cred.wifiPassword = WIFI_PASSWORD;
        cred.mqttHost = MQTT_HOST;
        cred.mqttToken = MQTT_TOKEN;
        cred.origen = "build_flags";
        return cred;
    }

    bool a = false, b = false, c = false, d = false;
    cred.wifiSsid     = _elegir(nvs, "wifi_ssid",  WIFI_SSID,     a);
    cred.wifiPassword = _elegir(nvs, "wifi_pass",  WIFI_PASSWORD, b);
    cred.mqttHost     = _elegir(nvs, "mqtt_host",  MQTT_HOST,     c);
    cred.mqttToken    = _elegir(nvs, "mqtt_token", MQTT_TOKEN,    d);
    nvs.end();

    if (a && c && d)            cred.origen = "NVS";
    else if (!a && !c && !d)    cred.origen = "build_flags";
    else                        cred.origen = "mixto (NVS + build_flags)";

    return cred;
}
