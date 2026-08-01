#include "MQTTManager.h"

#include <LittleFS.h>

#include "../config.h"
#include "../core/Credenciales.h"

using core::ResultadoPublicacion;

MQTTManager::MQTTManager(WiFiClientSecure& tlsClient) : _client(tlsClient) {}

bool MQTTManager::_loadCACertificate(const char* path) {
    File cert = LittleFS.open(path, "r");
    if (!cert) {
        LOG_E("MQTT", "No se pudo abrir el certificado CA: %s", path);
        LOG_E("MQTT", "Ejecutar: 'pio run --target uploadfs' para flashear data/certs/");
        return false;
    }

    // El certificado se copia a un buffer propio y persistente. `setCACert()`
    // NO copia la cadena: guarda el puntero. Antes se le pasaba el `c_str()` de
    // un `String` local que se destruía al salir de esta función, así que
    // mbedTLS parseaba memoria liberada durante el handshake. Que funcionara
    // dependía de que nadie hubiera reutilizado aún ese trozo de heap.
    _caPem = cert.readString();
    cert.close();

    if (_caPem.length() < 100 || _caPem.indexOf("-----BEGIN CERTIFICATE-----") < 0) {
        LOG_E("MQTT", "'%s' no contiene un certificado PEM (%u bytes).",
              path, (unsigned)_caPem.length());
        _caPem = String();
        return false;
    }

    _client.setCACert(_caPem.c_str());
    LOG_I("MQTT", "Certificado CA cargado (%u bytes).", (unsigned)_caPem.length());
    return true;
}

void MQTTManager::begin(const char* host, uint16_t port,
                        const char* username, const char* password,
                        const char* clientId) {
    // Copia propia: `host` y `password` vienen de un `String` cargado de NVS
    // cuyo ciclo de vida no controlamos, y las librerías guardan el puntero.
    _hostStr = host != nullptr ? host : "";
    _passwordStr = password != nullptr ? password : "";
    _host = _hostStr.c_str();
    _port = port;
    _username = username;
    _password = _passwordStr.c_str();
    _clientId = clientId;

    _credencialesOk = core::credencialValida(std::string(_hostStr.c_str())) &&
                      core::credencialValida(std::string(_passwordStr.c_str()));
    if (!_credencialesOk) {
        LOG_E("MQTT", "Nodo sin aprovisionar (host o token ausentes): no se conectara.");
    }

    // TLS 1.2.
    //
    // `WiFiClientSecure` del core Arduino-ESP32 NO expone un selector de
    // versión mínima (`setMinSupportedTLS` es API de ESP8266/BearSSL y aquí no
    // existe: su uso impedía compilar). No hace falta: mbedTLS se compila en
    // ESP-IDF con TLS 1.2 como único protocolo habilitado, así que el suelo ya
    // es 1.2 y no hay downgrade posible a 1.0/1.1.
    //
    // La garantía que sí depende de nosotros es la autenticación del servidor:
    // `setCACert()`. Sin ella el handshake aceptaría cualquier certificado, que
    // es el riesgo real (MITM), no la versión del protocolo.
    _client.setTimeout(TLS_HANDSHAKE_TIMEOUT_MS / 1000);
    _client.setHandshakeTimeout(TLS_HANDSHAKE_TIMEOUT_MS / 1000);

    // Cargar certificado CA desde LittleFS.
    //
    // Si falla, el nodo NO intenta conectar (ver `connect()`). El firmware no
    // llama a `setInsecure()` en ningún punto y no debe hacerlo: sin validación
    // de la CA, el handshake acepta cualquier certificado y la telemetría de la
    // cadena de frío queda expuesta a un MITM trivial. Preferimos un nodo que
    // acumula en LittleFS y lo grita por serie a uno que publica sin cifrado
    // verificado (RNF-05, ISVS-CRYPT-02).
    _caCargado = _loadCACertificate(CERT_FILE);
    if (!_caCargado) {
        LOG_E("MQTT", "Sin certificado CA valido: no se publicara nada.");
        LOG_E("MQTT", "Las lecturas siguen guardandose en LittleFS.");
    }

    // `begin(hostname, port, client)` conecta por NOMBRE, no por IP: es lo que
    // hace que `WiFiClientSecure` envíe el SNI en el handshake TLS y que EMQX
    // presente el certificado correcto.
    _mqtt.begin(_host, _port, _client);
    _mqtt.onMessage(_mqttCallback);

    // Keep-alive: plazo tras el cual EMQX da el nodo por muerto y publica su
    // LWT. Es la latencia de detección de una caída (§3.6).
    // Command timeout: cuánto se espera un CONNACK o un PUBACK antes de darlo
    // por perdido. Entra directamente en el presupuesto del watchdog.
    _mqtt.setOptions(MQTT_KEEPALIVE_SEC, /*cleanSession=*/true, MQTT_COMMAND_TIMEOUT_MS);

    // LWT registrado ANTES del CONNECT: si el ESP32 se apaga sin enviar
    // DISCONNECT, el broker publica esto automáticamente.
    _mqtt.setWill(TOPIC_LWT, LWT_PAYLOAD_OFFLINE, MQTT_RETAIN != 0, MQTT_QOS);

    LOG_I("MQTT", "Configurado: %s:%d (TLS 1.2, publicacion QoS %d con PUBACK, LWT QoS %d).",
          _host, _port, MQTT_QOS, MQTT_QOS);
}

bool MQTTManager::connect() {
    if (_mqtt.connected()) return true;

    // Fallo cerrado: sin CA no hay TLS verificable, y sin credenciales el
    // broker rechaza la conexión de todos modos. En ambos casos reintentar en
    // bucle solo gasta energía y llena el log.
    if (!_caCargado) return false;
    if (!_credencialesOk) return false;

    const unsigned long ahora = millis();
    if ((unsigned long)(ahora - _lastConnectAttempt) < RECONNECT_COOLDOWN_MS) {
        return false;
    }
    _lastConnectAttempt = ahora;

    LOG_I("MQTT", "Conectando a %s:%d como '%s'...", _host, _port, _clientId);

    if (!_mqtt.connect(_clientId, _username, _password)) {
        // `lastError()` es el error de transporte/protocolo de lwmqtt;
        // `returnCode()` es el código de rechazo del propio broker. Antes solo
        // se registraba un número de PubSubClient que mezclaba ambos planos.
        LOG_E("MQTT", "Conexión fallida. lastError=%d, returnCode=%d",
              (int)_mqtt.lastError(), (int)_mqtt.returnCode());
        switch (_mqtt.returnCode()) {
            case LWMQTT_IDENTIFIER_REJECTED:
                LOG_E("MQTT", "  → Client ID rechazado por el broker."); break;
            case LWMQTT_SERVER_UNAVAILABLE:
                LOG_E("MQTT", "  → Broker no disponible."); break;
            case LWMQTT_BAD_USERNAME_OR_PASSWORD:
                LOG_E("MQTT", "  → Credenciales inválidas (device_id / token)."); break;
            case LWMQTT_NOT_AUTHORIZED:
                LOG_E("MQTT", "  → No autorizado: revisar la regla ACL en EMQX."); break;
            default:
                LOG_E("MQTT", "  → Sin CONNACK: ¿handshake TLS o red? Revisar la CA."); break;
        }
        return false;
    }

    LOG_I("MQTT", "Conectado a EMQX. Publicando evento 'online'...");

    // Nada más conectar, avisar de que estamos vivos. Ahora en QoS 1: si el
    // broker no lo confirma, el backend no marcaría el nodo como conectado y
    // conviene saberlo.
    if (publicarEvento(LWT_PAYLOAD_ONLINE) != ResultadoPublicacion::Confirmado) {
        LOG_E("MQTT", "El broker no confirmó el evento 'online'.");
    }

    LOG_I("MQTT", "Listo. Publicando en '%s'.", TOPIC_LECTURAS);
    return true;
}

bool MQTTManager::isConnected() {
    return _mqtt.connected();
}

bool MQTTManager::loop() {
    return _mqtt.loop();
}

core::ResultadoPublicacion MQTTManager::publicarLectura(const char* topic, const char* payload) {
    if (!_mqtt.connected()) {
        LOG_E("MQTT", "No conectado. Imposible publicar.");
        return ResultadoPublicacion::SinConexion;
    }

    // GARANTÍA DE ENTREGA — QoS 1 con PUBACK verificado.
    //
    // `MQTTClient::publish()` con qos=1 delega en `lwmqtt_publish()`, que tras
    // enviar el PUBLISH espera el PUBACK dentro del command timeout y comprueba
    // que el packetId coincida con el emitido. Devuelve `true` solo entonces.
    // Es decir: `Confirmado` significa que el broker acusó recibo, no que el
    // paquete salió por el socket.
    //
    // Con eso la entrega nodo→broker es *at-least-once* (HU-11). El reenvío por
    // reintento es inofensivo: el backend deduplica por
    // UNIQUE(device_id, timestamp).
    const bool ok = _mqtt.publish(topic, payload, (int)strlen(payload),
                                  /*retained=*/false, MQTT_QOS);
    if (!ok) {
        LOG_E("MQTT", "Sin PUBACK (lastError=%d). Se conserva la lectura.",
              (int)_mqtt.lastError());
        // lwmqtt cierra la conexión ante un error de publicación, así que se
        // distingue "no hay sesión" de "el broker no confirmó": el llamador
        // detiene el drenaje en ambos casos, pero el log no es el mismo.
        return _mqtt.connected() ? ResultadoPublicacion::Fallo
                                 : ResultadoPublicacion::SinConexion;
    }

    return ResultadoPublicacion::Confirmado;
}

core::ResultadoPublicacion MQTTManager::publicarEvento(const char* eventJson) {
    return publicarLectura(TOPIC_EVENTOS, eventJson);
}

// =========================================================================
// Callback estático — el nodo no se suscribe a ningún topic
// =========================================================================
void MQTTManager::_mqttCallback(String& topic, String& payload) {
    (void)payload;
    LOG_I("MQTT", "Mensaje recibido en topic inesperado: %s (ignorado).", topic.c_str());
}
