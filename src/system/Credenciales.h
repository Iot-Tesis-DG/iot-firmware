#ifndef SYSTEM_CREDENCIALES_H
#define SYSTEM_CREDENCIALES_H

#include <Arduino.h>

/**
 * Carga de credenciales en tiempo de ejecución (RNF-05).
 *
 * Precedencia: NVS > build_flags > vacío.
 *
 * NVS va primero porque es lo que permite aprovisionar cada nodo por separado
 * sin recompilar ni tener el token en ningún archivo del repositorio. Las
 * `build_flags` se conservan como reserva para el banco de pruebas y para el
 * CI, donde no hay NVS que sembrar.
 *
 * Aprovisionamiento por NVS, una vez por dispositivo:
 *
 *   pio run --target upload                 # firmware sin credenciales
 *   # y desde una consola serie del propio nodo, o con `nvs_partition_gen.py`:
 *   #   namespace "thermotrace":
 *   #     wifi_ssid, wifi_pass, mqtt_host, mqtt_token
 *
 * `Preferences` guarda en la partición `nvs`, ya declarada en
 * `partitions_thermotrace.csv`.
 */
struct CredencialesNodo {
    String wifiSsid;
    String wifiPassword;
    String mqttHost;
    String mqttToken;

    /// ¿Hay lo mínimo para intentar conectar? Ver `core::aprovisionamientoCompleto`.
    bool completas() const;

    /// De dónde salió cada valor, para el log de arranque. Nunca imprime el
    /// valor en sí: el monitor serie de un prototipo acaba pegado en un anexo.
    String origen;
};

/// Lee las credenciales de NVS y, para lo que falte, de las `build_flags`.
CredencialesNodo cargarCredenciales();

#endif  // SYSTEM_CREDENCIALES_H
