# Cambios de la auditoría del backlog de 51 HU finales (2026-08-30) — iot-firmware

Este documento registra, repo por repo, lo implementado en `iot-firmware` para
cerrar las brechas encontradas en la auditoría del backlog final de 51
historias de usuario frente al estado real del código. Complementa
`backend/CAMBIOS_AUDITORIA_51HU.md` — varias de estas historias son
contratos de punta a punta entre firmware y backend; cuando lo son, se indica
explícitamente qué lado de `payload_schema.py` / `main.py` las recibe.

Todo lo descrito aquí está **compilado y probado con el toolchain real**, no
solo revisado a mano:

- **Host (`pio test -e native`, lógica pura de `src/core/`):** 87/87 pruebas
  en verde (compilado con MSVC `cl.exe` /std:c++17, ya que este entorno de
  desarrollo no tenía `gcc`/`g++` instalados; el propio `platformio.ini` fija
  `-std=gnu++17` para `native`, así que el estándar coincide).
- **Cross-compile real (`pio run -e esp32dev`):** build completo con
  `platform = espressif32@7.0.1`, `toolchain-xtensa-esp32@8.4.0`,
  `framework-arduinoespressif32`, `256dpi/MQTT`, `DallasTemperature`,
  `Adafruit SHT31`, etc. — **SUCCESS**, RAM 14.4 %, Flash 79.9 % de un
  ESP32 DevKitC de 4 MB. Esto compiló, entre otros, todos los archivos que
  dependen de Arduino (`MQTTManager`, `WiFiManager`, `main.cpp`,
  `PayloadBuilder`, `LittleFSBuffer`) — nada de eso corre en el entorno
  `native`, que solo compila `src/core/`.
- El cross-compile real **encontró y forzó a corregir un bug real** que el
  entorno `native` (MSVC, C++17) no detectó: ver la nota en HU-06 más abajo.
- No se ejecutó sobre hardware físico (no hay un ESP32 conectado a este
  entorno): falta la validación de campo de HU-08/HU-09 con Wi-Fi/broker
  reales, y de HU-04 con un MC-38 físicamente ausente.
- Quedó un `.pio-venv/` (venv de Python con PlatformIO) y el `.pio/` de
  build en el repo — ambos ya cubiertos por `.gitignore` (`.pio/` y, por
  coincidencia de patrón, `.pio-venv/`). Se pueden borrar sin perder nada;
  se dejaron para que un siguiente build no tenga que reinstalar PlatformIO.

---

## HU-05 — Contrato de payload: `schema_version`, `reading_id`, estado por sensor

- `core::PayloadCore.h/.cpp`: nuevo `SCHEMA_VERSION_PAYLOAD = 1`; `Lectura` gana `readingId`, `estadoTemperaturaInterna/Ambiental/HumedadAmbiental` (`core::EstadoSensor`: `Ok` / `SensorError` / `FueraDeRango`, con `nombreEstadoSensor()`); `serializarLectura()` ahora emite `schema_version`, `reading_id` y `estado_temperatura_interna/ambiental`/`estado_humedad_ambiental` junto a cada valor.
- `payload::PayloadBuilder`: `setTemperatureInterna/Ambiental/HumidityAmbiental` ganan un parámetro `core::EstadoSensor estado = Ok` (compatible hacia atrás en la firma — todas las llamadas existentes sin ese argumento siguen compilando). `build()` calcula `reading_id = deviceId + "_" + timestamp` justo antes de serializar — reutiliza device_id+timestamp porque ya es la clave de deduplicación del backend, sin necesitar contador ni RTC con persistencia propia.
- `main.cpp` (`taskSensores`): deriva `EstadoSensor` de cada sensor a partir de `isConnected()` (avería real, el sensor dejó de responder) vs. `NAN` con el sensor conectado (fuera del rango físico de la hoja de datos, ver `core/RangosSensores.h`) — antes esta distinción no llegaba al backend, todo era `null` sin causa.
- Lado backend: `LecturaPayload` ya declaraba `schema_version`/`reading_id` como opcionales; con este cambio el firmware real los emite. `estado_temperatura_*` ya estaba validado por `_estado_acompana_null_correctamente` en `payload_schema.py`.
- Pruebas nuevas en `test/test_core/test_payload.cpp`: `test_payload_coincide_con_el_documentado` (golden test) actualizado con el JSON completo nuevo; `test_schema_version_y_reading_id_se_emiten`, `test_estado_por_sensor_ok_por_defecto`, `test_estado_por_sensor_distingue_averia_de_fuera_de_rango`.

## HU-04 — Dispositivo sin sensor MC-38 (puerta)

- `core::Lectura` gana `mc38Instalado` (bool, default `true`). `serializarLectura()`: si `!mc38Instalado`, emite `"apertura_refrigerador":null,"mc38_status":"not_installed"` en vez de simular una puerta cerrada que en realidad no existe; si está instalado, emite el valor real + `"mc38_status":"ok"`.
- `PayloadBuilder::setMc38Instalado(bool)` (nuevo setter).
- `config.h`: nuevo build flag `MC38_INSTALADO` (default `1`). Un nodo sin el sensor se compila con `-DMC38_INSTALADO=0`.
- `main.cpp`: con `MC38_INSTALADO=0` no se llama a `mc38.begin()`, `mc38.poll()`, `mc38.huboApertura()`/`duracionAperturaSegundos()` ni `mc38.limpiarReporte()` — no es solo "no reportar", es "no muestrear en absoluto". Motivo: con el pull-up interno del GPIO, un pin sin sensor conectado flota en HIGH, el mismo nivel que "puerta abierta"; sin este flag el firmware reportaría una apertura constante y falsa en vez de "no aplica".
- Lado backend (ver `backend/CAMBIOS_AUDITORIA_51HU.md`, sección HU-04): `apertura_refrigerador` pasó de `bool` a `bool | None` en todo el pipeline (`LecturaPayload`, `LecturaTermica`, `ThermalReadingModel`, `LecturaResponse`), con migración `0012_hu04_mc38_ausente`; `ClasificarRiesgoTermicoUseCase` trata `None` como `False` neutro para el vector de features de la IA (hecho de configuración de hardware, no dato faltante); `generador_pdf.py` distingue "Abierta"/"Cerrada"/"N/D (sin MC-38)" en el reporte.
- Pruebas nuevas: `test_mc38_no_instalado_emite_null_y_mc38_status`, `test_mc38_instalado_emite_mc38_status_ok`.

## HU-07 — Acuse lógico de aplicación antes de borrar el buffer offline

Antes: el archivo de LittleFS se borraba con solo el PUBACK de transporte confirmado. El PUBACK únicamente certifica que el **broker** recibió el mensaje, no que el **backend** lo persistió — una lectura podía perderse si el backend caía o rechazaba el mensaje justo después del PUBACK.

- `core::extraerCampoString(json, campo)` (nuevo, en `PayloadCore.h/.cpp`): extractor de un campo string de un JSON plano de un solo nivel — no es un parser JSON general (no soporta anidamiento ni comillas escapadas en el valor), es lo mínimo para leer `reading_id` del propio payload y del acuse. Se prueba en host (`test_extraer_campo_string_*`).
- `config.h`: `TOPIC_ACK` (`farmacias/{DEVICE_ID}/ack`) y `ACK_LOGICO_TIMEOUT_MS` (8000 ms — 60 % más que `MQTT_COMMAND_TIMEOUT_MS` porque el acuse llega DESPUÉS del PUBACK, no en paralelo).
- `MQTTManager`:
  - Se suscribe a `TOPIC_ACK` justo después de conectar (dentro de `connect()`).
  - Callback (`_mqttCallback` → `_onMessage`, ahora con estado de instancia vía un puntero estático `_instancia`, porque la librería `256dpi/MQTT` no permite pasar contexto de usuario al callback): guarda el `reading_id` del último acuse recibido.
  - `esperarAckLogico(readingId, timeoutMs)` (nuevo): bombea `mqtt.loop()` en un bucle que alimenta el watchdog cada 20 ms hasta ver el acuse para ESE `reading_id` o expirar el timeout. Contempla la carrera donde el acuse ya llegó DURANTE el propio `publish()` (lwmqtt puede procesar mensajes entrantes mientras espera el PUBACK): comprueba el estado pendiente antes de descartarlo.
- `main.cpp` (`PublicadorMQTT::publicar`): tras el PUBACK confirmado, extrae el `reading_id` del payload y llama a `esperarAckLogico`. Solo devuelve `Confirmado` (la única condición bajo la que `core::drenar()` borra el archivo) si AMBOS llegaron; si el acuse lógico no llega a tiempo, devuelve `Fallo` — el archivo se conserva y se reintenta.
- **Hallazgo en el lado backend** (ver su changelog): los rechazos PERMANENTES (`DispositivoNoAutorizadoError`, `LecturaInvalidaError`) no publicaban ningún acuse — el firmware habría reintentado esas lecturas para siempre, bloqueando toda la cola FIFO detrás de ellas (`core::drenar` se detiene en el primer fallo). Se corrigió en `backend/src/interface/main.py`: `_publicar_ack_lectura()` ahora acepta un `estado` (`"commit_confirmado"` | `"dispositivo_no_autorizado"` | `"lectura_invalida"`); el firmware no necesita interpretar cuál — solo dejar de reintentar al recibir cualquier acuse para su `reading_id`.
- Doc-comments actualizados para reflejar el contrato real: `core/ColaFIFO.h` (`ResultadoPublicacion::Confirmado` ya no es "solo PUBACK", depende del `Publicador` concreto), `system/Watchdog.h` (nueva fila en la tabla de bloqueos, marcada como alimentable).

## HU-06 — Persistencia en buffer offline: saturación reportada (criterio 3)

Antes: `ColaFIFO::guardar()` aplicaba FIFO en silencio al saturarse — ninguna traza de qué se perdió.

- `core::ColaFIFO`: nuevo `ResumenSaturacion { descartadas, desde, hasta }` y `tomarResumenSaturacion()` (consume y resetea). Antes de borrar cada archivo por saturación, se lee su contenido y se extrae su `timestamp` (mismo `extraerCampoString` de HU-07) para acumular el periodo afectado.
- `LittleFSBuffer::tomarResumenSaturacion()`: delega bajo el mismo mutex que ya protege `_cola` (Core 0 escribe, Core 1 lee — sin este mutex el contador de saturación tendría una carrera igual que el resto de la cola).
- `main.cpp` (`reportarSaturacionSiHubo`, llamado cada vuelta de `taskRed`): registra SIEMPRE en el log local (`LOG_E`, incondicional — ninguna pérdida queda silenciosa aunque el nodo esté sin red en ese momento) y, si hay conexión, publica un evento `buffer_saturado` con el conteo y el periodo en `detalle`.
- Backend: nuevo `TipoEventoDispositivo.BUFFER_SATURADO` en `payload_schema.py`, nuevo `case` en `main.py::_procesar_evento_mqtt` (audita como `BUFFER_SATURADO`, SSE `saturacion_buffer`).
- **Bug real encontrado por el cross-compile a ESP32** (no por el entorno `native`/MSVC, que sí compiló esto sin quejarse): `ColaFIFO::ResumenSaturacion resumen{a, b, c};` — inicialización por agregado de una struct con inicializadores de miembro por defecto. MSVC con `/std:c++17` lo acepta; `xtensa-esp32-elf-g++ 8.4.0` (el compilador REAL del proyecto para el target) lo rechaza porque el framework Arduino-ESP32 no compila en el mismo estándar que asume esa forma de agregado. Se corrigió a asignación campo a campo. Sin el cross-compile real este bug habría llegado a un ESP32 físico sin que ninguna prueba lo detectara.
- Pruebas nuevas: host (`test_cola_fifo.cpp`) `test_sin_saturacion_el_resumen_esta_vacio`, `test_saturacion_registra_conteo_y_periodo_de_lo_descartado`, `test_tomar_resumen_resetea_el_contador`; backend `test_buffer_saturado_se_audita_con_periodo`.

## HU-08 — Reconexión automática: señal de diagnóstico (criterio 3)

Los criterios 1 y 2 (backoff progresivo con tope, reinicio del contador al reconectar) ya estaban implementados en `core::Backoff` / `WiFiManager`. Faltaba el criterio 3: una señal observable cuando los reintentos superan lo operativamente normal.

- `config.h`: `WIFI_UMBRAL_DIAGNOSTICO_INTENTOS = 5` (≈31 s de backoff acumulado, más allá de una caída momentánea del AP).
- `WiFiManager`: `diagnosticoActivo()` (se activa al superar el umbral, con un log distintivo una sola vez — no en cada intento, para no ahogarlo en ruido); `consumirRecuperacionDeDiagnostico(RecuperacionDiagnostico&)` (true una única vez, al reconectar tras un episodio de diagnóstico, con `intentosFallidos` y `duracionMs`).
- `main.cpp`: al reconectar MQTT tras un episodio de diagnóstico, publica un evento `wifi_reconexion_prolongada` con el conteo y la duración — es el primer momento en que HAY red para avisar; no se puede reportar antes porque, precisamente, no hay conexión.
- Backend: nuevo `TipoEventoDispositivo.WIFI_RECONEXION_PROLONGADA`, nuevo `case` (audita como `WIFI_RECONEXION_PROLONGADA`, SSE `reconexion_prolongada`).
- La captura offline (Core 0 / LittleFS) no depende de esto ni se detiene por esto en ningún punto — es una señal adicional, no un requisito para seguir capturando.
- Prueba nueva backend: `test_wifi_reconexion_prolongada_se_audita`. `WiFiManager` depende del SDK de Wi-Fi del ESP32 y no es host-testable (igual que ya era el caso antes de este cambio); se verificó por revisión manual y por el cross-compile real a `esp32dev`.

## HU-13 — Estado de conectividad: desconexión ordenada (criterio 2)

Los criterios 1 y 3 (LWT al vencer el keep-alive; evento "online" al reconectar) ya estaban implementados. El criterio 2 (un DISCONNECT ordenado no debe interpretarse como caída abrupta) no tenía ningún punto del código que alguna vez enviara un DISCONNECT deliberado — sin ese caso de uso, no había nada que corregir ni que probar de forma no artificial.

- `MQTTManager::desconectarOrdenadamente()` (nuevo): envía un DISCONNECT MQTT limpio (`_mqtt.disconnect()`) en vez de simplemente dejar de usar la sesión. Un DISCONNECT ordenado hace que el broker NO dispare el LWT — a diferencia de perder la conexión (Wi-Fi caída, corte de energía), donde SÍ debe dispararse.
- Punto de uso real: la rotación de credenciales de HU-44 (ver abajo) es el primer caso legítimo de cierre deliberado de sesión en este firmware, y usa `desconectarOrdenadamente()` antes de reconectar con el token nuevo.

## HU-44 — Rotación de credenciales sin reinicio completo (escenario 2)

El escenario 1 (revocación → el broker rechaza CONNECT, código `LWMQTT_NOT_AUTHORIZED`) y el escenario 3 (auditoría encadenada con hash) ya estaban resueltos del lado backend (ver `backend/CAMBIOS_AUDITORIA_51HU.md`, HU-44/HU-10). Faltaba el escenario 2 del lado firmware: reconectar con un token nuevo sin `ESP.restart()`.

- `MQTTManager::actualizarCredenciales(password)` (nuevo): reemplaza el token que usará el PRÓXIMO `connect()`, sin recrear el objeto ni recargar el certificado CA.
- `config.h`: `MQTT_TOKEN_POLL_INTERVAL_MS = 300000` (5 min) — compromiso entre "el corte de acceso surte efecto pronto" y "no abrir `Preferences`(NVS) en cada vuelta del bucle de 100 ms".
- `main.cpp` (`revisarRotacionCredenciales`, llamada cada 5 min con sesión MQTT viva): relee `cargarCredenciales()` de NVS; si el token cambió, hace `mqtt.desconectarOrdenadamente()` (HU-13) + `mqtt.actualizarCredenciales(nuevo)` — la reconexión ocurre en la siguiente vuelta del bucle por el camino normal de "Mantener MQTT", con su backoff habitual.
- **Limitación real, no resuelta aquí:** esto detecta un token YA escrito en NVS (por un técnico, vía consola serie, con el nodo encendido) — no existe en este repositorio un mecanismo de aprovisionamiento remoto que empuje el token nuevo al dispositivo por red (tiene sentido: el propio token es el secreto de autenticación, no se puede pedir el nuevo usando el viejo ya revocado). El endpoint backend de rotación (`RotarCredencialDispositivoUseCase`) genera y hashea el token nuevo, pero entregarlo físicamente al nodo sigue siendo un paso operativo fuera del alcance de este firmware.
- No hay prueba de host para esto (depende de `Preferences`/NVS, no portable); se verificó por revisión manual y por el cross-compile real.

## No tocado en esta pasada

- **HU-09 (TLS/validación del broker):** ya implementado (ver docstring de `MQTTManager.h`); no se encontró brecha frente al backlog de 51 HU.
- **HU-11 (QoS 1 con PUBACK):** ya implementado; HU-07 lo extiende (acuse lógico), no lo reemplaza.
- **Validación en hardware real:** todo lo anterior está compilado (host + cross-compile real a `esp32dev`) pero no ejecutado sobre un ESP32 físico ni contra un broker EMQX real. Pendiente para la fase de validación de campo de la tesis.

## Cómo se verificó (para reproducir)

```
# Host — lógica pura de src/core/, 87 pruebas
pio test -e native

# Cross-compile real al target (descarga toolchain xtensa-esp32 + framework Arduino la primera vez)
pio run -e esp32dev
```

En este entorno de desarrollo no había `gcc`/`g++` ni PlatformIO preinstalados: se usó `cl.exe` de Visual Studio 2022 (con `/std:c++17`, igual que exige `platformio.ini` para `native`) para las 87 pruebas de host, y un venv de Python con `pip install platformio` para el cross-compile real a `esp32dev`, que sí usa el toolchain `xtensa-esp32-elf-g++` real del proyecto.
