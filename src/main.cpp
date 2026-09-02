/**
 * ThermoTrace — Firmware ESP32 para Monitoreo de Cadena de Frío
 * ===================================================================
 * Tesis UPC 2026: Desarrollo y validación de un prototipo basado en IoT e IA
 * con trazabilidad digital verificable para monitoreo de cadena de frío de
 * medicamentos termolábiles en farmacias independientes de Lima Metropolitana.
 *
 * Arquitectura dual-core (FreeRTOS en ESP32):
 *   Core 0 (Protocol Core):  Captura de sensores cada 30s + escritura LittleFS.
 *   Core 1 (Application Core): Loop principal: Wi-Fi + MQTT + sincronización.
 *
 * Flujo de datos:
 *   1. Sensores (DS18B20, SHT31, MC-38) → lectura cada 30s
 *   2. PayloadBuilder → JSON canónico (~250 bytes)
 *   3. RAM buffer → LittleFS (si no hay red, política FIFO)
 *   4. MQTT/TLS en QoS 1 (PUBACK del broker) → EMQX → backend
 *   5. Solo con el PUBACK Y el acuse LÓGICO del backend (HU-07, tras su
 *      COMMIT en PostgreSQL) → se libera el archivo de LittleFS
 *
 * Pines (ESP32 DevKitC V4):
 *   GPIO4  → DS18B20 (1-Wire, pull-up 4.7kΩ a 3.3V)
 *   GPIO21 → SHT31 SDA (I2C)
 *   GPIO22 → SHT31 SCL (I2C)
 *   GPIO15 → MC-38 (reed switch, pull-up interno)
 *
 * Requiere:
 *   - PlatformIO con framework arduino
 *   - Certificado CA raíz en data/certs/root_ca.pem
 *   - Credenciales Wi-Fi y MQTT en config.h o build_flags
 */

#include <Arduino.h>
#include <esp_task_wdt.h>

#include "config.h"
#include "sensors/DS18B20Sensor.h"
#include "sensors/SHT31Sensor.h"
#include "sensors/MC38Sensor.h"
#include "connectivity/WiFiManager.h"
#include "connectivity/MQTTManager.h"
#include "core/ColaFIFO.h"
#include "storage/LittleFSBuffer.h"
#include "payload/PayloadBuilder.h"
#include "system/Credenciales.h"
#include "system/Watchdog.h"

// =========================================================================
// Objetos globales (compartidos entre cores vía volatile / mutex)
// =========================================================================
DS18B20Sensor ds18b20(PIN_DS18B20);
SHT31Sensor   sht31(SHT31_I2C_ADDRESS);
MC38Sensor    mc38(PIN_MC38);
LittleFSBuffer buffer;

WiFiClientSecure tlsClient;
WiFiManager wifi;
MQTTManager  mqtt(tlsClient);

// Credenciales cargadas en `setup()` (NVS > build_flags). Global porque
// `MQTTManager` guarda punteros a sus cadenas durante toda la vida del proceso.
static CredencialesNodo credenciales;

// Contador informativo de lecturas capturadas desde la última publicación.
// Solo alimenta el log del Core 0; la cola real vive en LittleFS.
static volatile int ramBufferCount = 0;
static portMUX_TYPE ramMutex = portMUX_INITIALIZER_UNLOCKED;

// =========================================================================
// Core 0 — Tarea de sensores
// =========================================================================
void taskSensores(void* parameter) {
    LOG_I("Core0", "Tarea de sensores iniciada en Core %d.", xPortGetCoreID());
    suscribirTareaAlWatchdog("Sensores");

    TickType_t lastWakeTime = xTaskGetTickCount();
    const TickType_t intervalTicks = pdMS_TO_TICKS(INTERVALO_LECTURA_MS);

    for (;;) {
        // ── 1. Leer sensores ────────────────────────────────────────
        float tempInterna = ds18b20.readTemperatureC();
        float tempAmbiental = sht31.readTemperatureC();
        float humedad = sht31.readHumidity();

        // HU-15/HU-05: estado explícito por sensor. `isConnected()==false`
        // tras la lectura es una avería real (el sensor dejó de responder);
        // NAN con el sensor todavía conectado es una lectura fuera del rango
        // físico de la hoja de datos (ver core/RangosSensores.h) — el backend
        // necesita distinguir ambos casos, no solo ver `null`.
        core::EstadoSensor estadoInterna = core::EstadoSensor::Ok;
        if (isnan(tempInterna)) {
            estadoInterna = ds18b20.isConnected()
                ? core::EstadoSensor::FueraDeRango
                : core::EstadoSensor::SensorError;
        }
        core::EstadoSensor estadoAmbiental = core::EstadoSensor::Ok;
        core::EstadoSensor estadoHumedad = core::EstadoSensor::Ok;
        if (isnan(tempAmbiental) || isnan(humedad)) {
            // El SHT31 entrega ambos valores en una sola transacción I2C: un
            // fallo de lectura invalida los dos (ver SHT31Sensor.h).
            const core::EstadoSensor estadoSht31 = sht31.isConnected()
                ? core::EstadoSensor::FueraDeRango
                : core::EstadoSensor::SensorError;
            if (isnan(tempAmbiental)) estadoAmbiental = estadoSht31;
            if (isnan(humedad)) estadoHumedad = estadoSht31;
        }

        // La puerta se muestrea cada 50 ms durante toda la ventana (ver la
        // espera al final del ciclo), no aquí. Se reporta si hubo apertura en
        // algún momento de los 30 s, no si justo estaba abierta al muestrear:
        // una apertura de 15 s entre dos muestras se perdía por completo.
        //
        // HU-04: sin MC-38 instalado no se muestrea en absoluto — con el
        // pull-up interno, un pin sin sensor flota en el mismo nivel que
        // "abierta", así que el polling normal reportaría una apertura
        // constante y falsa en vez de "no aplica".
        bool puertaAbierta = false;
        unsigned long duracionPuerta = 0;
#if MC38_INSTALADO
        puertaAbierta = mc38.huboApertura();
        duracionPuerta = mc38.duracionAperturaSegundos();
#endif

        // ── 2. Construir payload JSON ───────────────────────────────
        PayloadBuilder payload(DEVICE_ID, FIRMWARE_VERSION);
        payload.setTemperatureInterna(tempInterna, estadoInterna);
        payload.setTemperatureAmbiental(tempAmbiental, estadoAmbiental);
        payload.setHumidityAmbiental(humedad, estadoHumedad);
        payload.setDoorOpen(puertaAbierta, duracionPuerta);
        payload.setMc38Instalado(MC38_INSTALADO);
        payload.setConnectivityOnline(wifi.isConnected());

        String json = payload.build(512);
        if (json.isEmpty()) {
            LOG_E("Core0", "Payload vacío — omitiendo ciclo.");
            vTaskDelayUntil(&lastWakeTime, intervalTicks);
            continue;
        }

        // ── 3. Guardar en LittleFS (siempre, offline-first) ─────────
        bool saved = buffer.saveReading(json.c_str());
        if (saved) {
            portENTER_CRITICAL(&ramMutex);
            ramBufferCount++;
            portEXIT_CRITICAL(&ramMutex);
        }

        // Ventana de reporte cerrada: lo que venga a partir de aquí cuenta
        // para el siguiente payload.
#if MC38_INSTALADO
        mc38.limpiarReporte();
#endif

        LOG_I("Core0", "Ciclo completado. Pendientes: %d en RAM, %d en Flash.",
              ramBufferCount, buffer.pendingCount());

        // ── 4. Esperar al próximo ciclo MUESTREANDO la puerta ───────
        //
        // La cadencia sigue anclada a `lastWakeTime`, así que el jitter no se
        // acumula y los 30 s del RF siguen siendo 30 s. Pero en vez de dormir
        // de un tirón, se despierta cada 50 ms para llamar a `mc38.poll()`:
        // sin esto el antirrebote no tiene muestras sobre las que actuar y las
        // aperturas que empiezan y terminan dentro de la ventana no existen.
        //
        // Solo la puerta necesita este trato: temperatura y humedad son
        // magnitudes lentas y muestrearlas cada 30 s es correcto.
        const TickType_t pasoPoll = pdMS_TO_TICKS(MC38Sensor::POLL_MS);
        for (;;) {
            // Diferencia con signo: sobrevive al desbordamiento del contador de
            // ticks (~49 días con tick de 1 kHz), donde una resta sin signo
            // daría un valor enorme y colgaría el muestreo.
            const int32_t faltan =
                (int32_t)(lastWakeTime + intervalTicks - xTaskGetTickCount());
            if (faltan <= (int32_t)pasoPoll) break;
            vTaskDelay(pasoPoll);
#if MC38_INSTALADO
            mc38.poll();
#endif
            alimentarWatchdog();
        }
        vTaskDelayUntil(&lastWakeTime, intervalTicks);
    }
}

// =========================================================================
// Core 1 — Tarea de red (Wi-Fi + MQTT + sincronización)
// =========================================================================
// Archivos publicados como máximo en una pasada del bucle de red.
//
// Con la cola llena (200 archivos) y un handshake TLS por delante, drenarla
// entera en una sola iteración deja de alimentar al watchdog: el propio manual
// avisa de que una operación bloqueante de más de 5 s reinicia el ESP32
// (§7.2). Se drena a ritmo acotado, cediendo CPU entre lotes; con lecturas
// cada 30 s, 20 por pasada vacían el backlog completo en pocos segundos.
static constexpr int MAX_PUBLICACIONES_POR_CICLO = 20;

/// Adaptador entre la política portable de drenaje (`core::drenar`) y el
/// cliente MQTT real. La política —qué se borra, cuándo se para, qué se
/// descarta— vive en `core/ColaFIFO.cpp` y se prueba en el host con
/// intercalados adversarios; aquí solo queda la llamada al broker.
class PublicadorMQTT : public core::Publicador {
public:
    core::ResultadoPublicacion publicar(const std::string& payload) override {
        // El watchdog se alimenta ANTES de cada publicación: cada PUBACK puede
        // bloquear hasta MQTT_COMMAND_TIMEOUT_MS y en un ciclo se encadenan
        // hasta MAX_PUBLICACIONES_POR_CICLO de ellas.
        alimentarWatchdog();
        const core::ResultadoPublicacion transporte =
            mqtt.publicarLectura(TOPIC_LECTURAS, payload.c_str());
        if (transporte != core::ResultadoPublicacion::Confirmado) {
            return transporte;
        }

        // HU-07: el PUBACK solo confirma que el BROKER recibió el mensaje,
        // no que el backend lo persistió. `Confirmado` —la única condición
        // bajo la que `core::drenar()` borra el archivo de LittleFS— exige
        // además el acuse LÓGICO de aplicación, publicado por el backend
        // recién después de hacer COMMIT (o de rechazar la lectura de forma
        // permanente; ver interface/main.py::_publicar_ack_lectura).
        const std::string readingId = core::extraerCampoString(payload, "reading_id");
        if (!mqtt.esperarAckLogico(readingId, ACK_LOGICO_TIMEOUT_MS)) {
            LOG_E("Core1", "PUBACK si, acuse logico no (reading_id=%s). Se conserva.",
                  readingId.c_str());
            return core::ResultadoPublicacion::Fallo;
        }
        return core::ResultadoPublicacion::Confirmado;
    }
};

/// Adaptador de `LittleFSBuffer` a la interfaz que espera `core::drenar()`.
///
/// Toma el mutex por operación, NUNCA durante la publicación: así el Core 0
/// nunca espera un PUBACK (hasta 5 s) para guardar su lectura de los 30 s, solo
/// una operación de flash.
class ColaDelBuffer : public core::AlmacenLecturas {
public:
    std::vector<std::string> listar() override {
        std::vector<std::string> r;
        for (const auto& f : buffer.listPendingFiles()) r.push_back(std::string(f.c_str()));
        return r;
    }
    bool existe(const std::string&) override { return false; }
    size_t escribir(const std::string&, const std::string&) override { return 0; }
    bool leer(const std::string& nombre, std::string& salida) override {
        const String contenido = buffer.readFile(String(nombre.c_str()));
        if (contenido.isEmpty()) return false;
        salida = std::string(contenido.c_str());
        return true;
    }
    bool borrar(const std::string& nombre) override {
        return buffer.removeFile(String(nombre.c_str()));
    }
};

/// Publica lecturas pendientes de LittleFS en orden FIFO.
///
/// Un archivo solo se borra tras el PUBACK confirmado del broker (QoS 1). Antes
/// se borraba tras un `publish()` de QoS 0 revalidando la sesión, lo que
/// acotaba la ventana de pérdida pero no la cerraba.
static int drenarBuffer() {
    static ColaDelBuffer almacen;
    static PublicadorMQTT publicador;
    core::ColaFIFO cola(almacen, LITTLEFS_MAX_FILES, LITTLEFS_MAX_FILESIZE);

    const core::ResumenDrenaje resumen =
        core::drenar(cola, publicador, MAX_PUBLICACIONES_POR_CICLO);

    if (resumen.descartados > 0) {
        LOG_E("Core1", "%d archivo(s) ilegible(s) o truncado(s) descartado(s).",
              resumen.descartados);
    }
    return resumen.confirmados;
}

/// HU-06 criterio 3: construye un evento de `/eventos` con `detalle` libre.
/// No hace falta un escapador JSON completo porque `detalle` aquí solo
/// contiene dígitos, texto fijo en español y timestamps ISO 8601 — ninguno
/// de los dos produce comillas ni backslashes.
static String construirEventoJSON(const char* tipoEvento, const String& detalle) {
    String json = "{\"device_id\":\"" DEVICE_ID "\",\"tipo_evento\":\"";
    json += tipoEvento;
    json += "\",\"timestamp\":\"";
    json += PayloadBuilder::timestampISO8601();
    json += "\",\"detalle\":\"";
    json += detalle;
    json += "\"}";
    return json;
}

/// HU-06 criterio 3: reporta lo que se perdió por saturación del buffer
/// desde la última pasada. El registro local (LOG_E) es incondicional —
/// ninguna pérdida queda silenciosa aunque el nodo esté sin conexión al
/// momento de perderla—; el evento a `/eventos` es best-effort, igual que
/// `ERROR_SENSOR`.
static void reportarSaturacionSiHubo() {
    const core::ColaFIFO::ResumenSaturacion saturacion = buffer.tomarResumenSaturacion();
    if (saturacion.descartadas <= 0) return;

    String detalle = String(saturacion.descartadas);
    detalle += " lectura(s) descartada(s) por saturacion del buffer offline, periodo ";
    detalle += saturacion.desde.c_str();
    detalle += " a ";
    detalle += saturacion.hasta.c_str();

    LOG_E("Core1", "%s", detalle.c_str());

    if (mqtt.isConnected()) {
        if (mqtt.publicarEvento(construirEventoJSON("buffer_saturado", detalle).c_str())
            != core::ResultadoPublicacion::Confirmado) {
            LOG_E("Core1", "El broker no confirmo el evento de saturacion (queda solo en el log local).");
        }
    }
}

/// HU-44 escenario 2: relee el token MQTT de NVS y, si cambió (rotación o
/// reaprovisionamiento aplicado por un técnico con el nodo encendido),
/// reconecta con la credencial nueva SIN `ESP.restart()`. El historial en
/// LittleFS/backend no se toca — es la misma sesión de captura, solo cambia
/// con qué credencial se publica de aquí en adelante.
static void revisarRotacionCredenciales() {
    const CredencialesNodo actualizadas = cargarCredenciales();
    if (actualizadas.mqttToken == credenciales.mqttToken) return;

    LOG_I("Core1", "Token MQTT rotado en NVS (origen=%s). Reconectando sin reinicio completo.",
          actualizadas.origen.c_str());

    // HU-13 criterio 2: DISCONNECT ordenado — el broker no debe interpretar
    // este cierre voluntario como una caída abrupta y disparar el LWT.
    mqtt.desconectarOrdenadamente();
    mqtt.actualizarCredenciales(actualizadas.mqttToken.c_str());
    credenciales = actualizadas;
    // La reconexión ocurre en la siguiente vuelta del bucle: tras el
    // DISCONNECT, `mqtt.isConnected()` ya es false, así que el paso 2
    // ("Mantener MQTT") de `taskRed` se encarga, con su backoff habitual.
}

void taskRed(void* parameter) {
    LOG_I("Core1", "Tarea de red iniciada en Core %d.", xPortGetCoreID());
    suscribirTareaAlWatchdog("Red");
    unsigned long ultimaRevisionCredenciales = millis();

    for (;;) {
        alimentarWatchdog();

        // ── 1. Mantener Wi-Fi ───────────────────────────────────────
        // Se alimenta el watchdog entre CADA etapa bloqueante. Sin esto, el
        // handshake TLS (10 s) y NTP (10 s) podrían encadenarse en una sola
        // iteración y superar el plazo, aunque cada uno por separado quepa.
        wifi.maintain();
        alimentarWatchdog();
        if (!wifi.isConnected()) {
            delay(500);
            continue;
        }

        // ── 2. Mantener MQTT ────────────────────────────────────────
        if (!mqtt.isConnected()) {
            const bool conectado = mqtt.connect();
            alimentarWatchdog();  // el handshake TLS + CONNACK son el peor tramo
            if (!conectado) {
                delay(1000);
                continue;
            }

            // ── 3. RE-CONEXIÓN ──────────────────────────────────────
            //
            // Aquí NO se publica ningún evento "online": ya lo hace
            // `MQTTManager::connect()` con `LWT_PAYLOAD_ONLINE`, que tiene la
            // forma que el backend espera en `/eventos` (`tipo_evento`).
            //
            // Antes se construía un `PayloadBuilder` —es decir, un JSON de
            // LECTURA— y se publicaba en el tópico de EVENTOS. El backend lo
            // valida con `EventoDispositivoPayload` (extra="forbid", exige
            // `tipo_evento`), así que fallaba siempre: cada reconexión
            // generaba un ValidationError descartado con un warning.
            //
            // Con la Wi-Fi ya levantada, este es además el momento de
            // recuperar el reloj si NTP no pudo sincronizar en el arranque.
            extern bool ntpEstaSincronizado();
            extern void syncNTP();
            if (!ntpEstaSincronizado()) {
                LOG_I("Core1", "Reintentando sincronización NTP...");
                syncNTP();
                alimentarWatchdog();  // getLocalTime() bloquea hasta 10 s
            }

            // HU-08 criterio 3: si la Wi-Fi venía de un episodio de
            // diagnóstico (reintentos por encima del umbral operativo), este
            // es el primer momento en que hay red para reportarlo — no se
            // podía avisar antes, precisamente porque no había conexión.
            WiFiManager::RecuperacionDiagnostico diagnostico;
            if (wifi.consumirRecuperacionDeDiagnostico(diagnostico)) {
                String detalle = "Reconectado tras ";
                detalle += diagnostico.intentosFallidos;
                detalle += " intento(s) fallido(s) (";
                detalle += diagnostico.duracionMs / 1000;
                detalle += " s sin red). Umbral de diagnostico: ";
                detalle += WIFI_UMBRAL_DIAGNOSTICO_INTENTOS;
                if (mqtt.publicarEvento(
                        construirEventoJSON("wifi_reconexion_prolongada", detalle).c_str())
                    != core::ResultadoPublicacion::Confirmado) {
                    LOG_E("Core1", "El broker no confirmo el evento de reconexion prolongada.");
                }
            }
        }

        // ── 4. Procesar keep-alive y callbacks ──────────────────────
        mqtt.loop();

        // ── 5. Publicar la cola pendiente ───────────────────────────
        //
        // Se ejecuta en CADA pasada con conexión, no solo al reconectar.
        // Antes vivía dentro del `if (!mqtt.isConnected())` de arriba, así que
        // en régimen normal —conectado— no se publicaba absolutamente nada:
        // el Core 0 seguía escribiendo en LittleFS cada 30 s, la cola crecía
        // hasta 200 y el FIFO empezaba a tirar las lecturas más antiguas. El
        // dashboard solo veía datos justo después de una reconexión.
        //
        // Camino único para telemetría en vivo y para el backlog offline: el
        // Core 0 siempre persiste antes (offline-first, sobrevive a un corte de
        // corriente) y el Core 1 publica en orden FIFO. Con el bucle a 100 ms,
        // una lectura recién guardada sale muy por debajo del techo de 5 s del
        // RNF-01.
        int enviados = drenarBuffer();
        if (enviados > 0) {
            LOG_I("Core1", "Confirmadas %d lecturas (PUBACK + acuse logico). Quedan %d en Flash.",
                  enviados, buffer.pendingCount());
            portENTER_CRITICAL(&ramMutex);
            ramBufferCount = 0;
            portEXIT_CRITICAL(&ramMutex);
        }

        // ── 6. Reportar saturación del buffer, si la hubo ───────────
        reportarSaturacionSiHubo();

        // ── 7. Revisar rotación de credenciales (HU-44) ─────────────
        //
        // Solo con sesión viva: si ya está reconectando por otra razón, el
        // paso 2 se encarga primero y esta revisión espera a la próxima
        // ventana de MQTT_TOKEN_POLL_INTERVAL_MS.
        if (mqtt.isConnected() &&
            (unsigned long)(millis() - ultimaRevisionCredenciales) >= MQTT_TOKEN_POLL_INTERVAL_MS) {
            ultimaRevisionCredenciales = millis();
            revisarRotacionCredenciales();
        }

        delay(100);  // Ceder tiempo al scheduler de FreeRTOS
    }
}

// =========================================================================
// setup() — Inicialización en Core 1
// =========================================================================
void setup() {
    Serial.begin(115200);
    delay(1000);  // Esperar a que el monitor serie se estabilice

    Serial.println();
    Serial.println("╔══════════════════════════════════════════════════════╗");
    Serial.println("║   ThermoTrace — Firmware IoT Cadena de Frío        ║");
    Serial.println("║   Tesis UPC 2026                                   ║");
    Serial.println("╠══════════════════════════════════════════════════════╣");
    Serial.printf ("║   Device ID : %-36s ║\n", DEVICE_ID);
    Serial.printf ("║   Firmware  : %-36s ║\n", FIRMWARE_VERSION);
    Serial.printf ("║   ESP32 SDK : %-36s ║\n", ESP.getSdkVersion());
    Serial.println("╚══════════════════════════════════════════════════════╝");
    Serial.println();

    // ── Watchdog ────────────────────────────────────────────────────
    // Antes de crear las tareas: ambas se suscriben nada más arrancar.
    inicializarWatchdog();

    // ── Credenciales (RNF-05) ───────────────────────────────────────
    credenciales = cargarCredenciales();
    Serial.printf("[Setup] Credenciales: origen=%s, aprovisionado=%s\n",
                  credenciales.origen.c_str(),
                  credenciales.completas() ? "si" : "NO");
    if (!credenciales.completas()) {
        LOG_E("Setup", "Nodo SIN aprovisionar. Capturara y almacenara en LittleFS,");
        LOG_E("Setup", "pero no publicara. Ver system/Credenciales.h.");
    }

    // ── Inicializar LittleFS ────────────────────────────────────────
    if (!buffer.begin()) {
        LOG_E("Setup", "LittleFS no disponible. Reiniciando en 5s...");
        delay(5000);
        ESP.restart();
    }

    // ── Inicializar sensores ────────────────────────────────────────
    //
    // Un sensor ausente NO detiene el arranque: el nodo sigue publicando con
    // ese campo en null y el resto de la cadena funciona. Pero debe quedar
    // dicho en el monitor serie, porque el síntoma aguas abajo —un null en el
    // dashboard— no distingue "sensor mal cableado" de "lectura no disponible".
    // Ambos drivers reintentan la detección en cada ciclo.
    ds18b20.begin();
    if (!sht31.begin()) {
        LOG_E("Setup", "SHT31 no detectado en 0x%02X: revisar SDA=GPIO%d / SCL=GPIO%d.",
              SHT31_I2C_ADDRESS, 21, 22);
    }
#if MC38_INSTALADO
    mc38.begin();
#else
    LOG_I("Setup", "MC-38 deshabilitado por build_flag (MC38_INSTALADO=0).");
#endif

    // ── Inicializar Wi-Fi (Core 1) ──────────────────────────────────
    wifi.configurar(credenciales.wifiSsid, credenciales.wifiPassword);
    wifi.begin();

    // ── Sincronizar reloj NTP ───────────────────────────────────────
    // DESPUÉS de levantar la Wi-Fi, no antes: NTP viaja por UDP/123 y sin red
    // `getLocalTime()` se limita a agotar sus 10 s de espera y fallar. Con el
    // orden anterior la sincronización NO podía funcionar en ningún arranque,
    // así que todos los timestamps salían de la hora de COMPILACIÓN más
    // `millis()`. Pasadas 48 h desde el flasheo, el backend empieza a
    // rechazarlos por `timestamp_demasiado_antiguo` (ANTIGUEDAD_MAXIMA) y el
    // nodo deja de registrar sin ningún error visible en el monitor serie.
    extern void syncNTP();
    if (wifi.isConnected()) {
        syncNTP();
    } else {
        // Sin red al arrancar se usa el fallback de compilación; el reloj se
        // corrige en el primer ciclo de red con conexión.
        LOG_E("Setup", "Sin Wi-Fi al arrancar: NTP se reintentará al conectar.");
    }

    // ── Inicializar MQTT ────────────────────────────────────────────
    mqtt.begin(credenciales.mqttHost.c_str(), MQTT_PORT, MQTT_USERNAME,
               credenciales.mqttToken.c_str(), MQTT_CLIENT_ID);

    // ── Crear tareas en núcleos separados ───────────────────────────
    // Core 0: Sensores (prioridad más alta: la captura no puede retrasarse).
    xTaskCreatePinnedToCore(
        taskSensores,        // Función
        "Sensores",          // Nombre
        8192,                // Stack size (8 KB, suficiente para JSON + sensores)
        nullptr,             // Parámetro
        2,                   // Prioridad (0-24, más alto = más prioritario)
        nullptr,             // Handle (no necesitamos)
        0                    // Core 0 — Protocol Core
    );

    // Core 1: Red (Wi-Fi + MQTT + sincronización).
    xTaskCreatePinnedToCore(
        taskRed,             // Función
        "Red",               // Nombre
        12288,               // Stack size (12 KB, TLS + MQTT necesitan más)
        nullptr,             // Parámetro
        1,                   // Prioridad (menor que sensores)
        nullptr,             // Handle
        1                    // Core 1 — Application Core
    );

    // ── El loop() principal queda vacío: todo corre en tasks ────────
    LOG_I("Setup", "Inicialización completa. Core 0 = sensores, Core 1 = red.");
    LOG_I("Setup", "MQTT → %s:%d (TLS 1.2, publicacion QoS 1 con PUBACK)",
          credenciales.mqttHost.c_str(), MQTT_PORT);
    LOG_I("Setup", "Tópico → %s", TOPIC_LECTURAS);
}

// =========================================================================
// loop() — Vacío. Las tareas de FreeRTOS manejan todo.
// =========================================================================
void loop() {
    // Nada. taskSensores (Core 0) y taskRed (Core 1) corren concurrentemente.
    //
    // WATCHDOG: ambas tareas SÍ están ahora suscritas al TWDT (ver
    // `system/Watchdog.h`), con el plazo subido a 30 s para tolerar los dos
    // bloqueos legítimos de `taskRed` —15 s de asociación Wi-Fi y 10 s de
    // NTP— y con `alimentarWatchdog()` sembrado en los bucles de sondeo. Antes
    // solo vigilaba a `loopTask`, es decir, a este `delay(1000)`: se alimentaba
    // siempre y no habría disparado nunca.
    //
    // Pendiente de validar sobre hardware real: el plazo de 30 s se eligió por
    // análisis de los bloqueos del código, no midiendo el peor caso observado.
    delay(1000);
}
