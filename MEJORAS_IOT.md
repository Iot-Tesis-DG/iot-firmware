# Auditoría y endurecimiento del firmware IoT

Revisión de los 19 archivos de `src/` del nodo edge ESP32 (ThermoTrace, tesis
UPC 2026), con las correcciones aplicadas y las pruebas que las cubren.

Punto de partida: 1642 líneas de C++ sin ninguna prueba automatizada. El CI solo
comprobaba que el binario enlazara. Todo lo que este documento llama "defecto"
se refiere al estado del repositorio antes de esta revisión.

Se hizo en dos rondas. La segunda cerró lo que la primera dejó pendiente:
QoS 1 real (HU-11), versión de toolchain fijada, rama muerta del watchdog
retirada, plazo del watchdog justificado numéricamente y pruebas de concurrencia.
Las secciones marcadas "2.ª ronda" son de esa segunda pasada.

**Alcance de la verificación.** `pio run -e esp32dev` compila y `pio test -e
native` pasa (76 casos). Nada de esto se ha validado sobre hardware real: no ha
habido ESP32, ni sensores, ni broker EMQX en ningún momento. Las secciones
marcadas como *sin validar en hardware* señalan dónde eso importa más.

---

## 1. Concurrencia entre núcleos

### 1.1 El buffer LittleFS se comparte entre los dos cores sin sincronización — **crítico**

`taskSensores` (Core 0) escribe una lectura cada 30 s con `saveReading()`.
`taskRed` (Core 1) recorre, lee y borra la misma cola con `listPendingFiles()`,
`readFile()` y `removeFile()`. No había mutex, semáforo ni cola de por medio, y
ninguna de esas operaciones es atómica:

- `_nextFileIndex()` listaba el directorio y devolvía último+1. Si el Core 1
  borraba el último archivo entre el listado y el `open()`, el índice se
  reutilizaba y la escritura pisaba una lectura todavía no publicada. Se pierde
  un dato de la cadena de frío sin ninguna traza.
- `openNextFile()` mantiene un cursor de directorio abierto mientras el otro
  núcleo crea o borra entradas en ese mismo directorio.

Por qué importaba: es la única estructura que ambas tareas tocan, y es
precisamente la que guarda la evidencia que la tesis quiere demostrar íntegra.

**Cambio.** `LittleFSBuffer` toma un mutex recursivo de FreeRTOS
(`xSemaphoreCreateRecursiveMutex`) en todos sus métodos públicos, con un guarda
RAII (`GuardaBuffer`) para que se libere también en los `return` por error de
flash. Recursivo porque `saveReading()` llama a `pendingCount()` y a
`removeFile()`, que también lo toman. El contador de índices pasa a mantenerse
en RAM, sembrado desde disco en `begin()`.

Archivos: `src/storage/LittleFSBuffer.{h,cpp}`.

*Sin validar en hardware.* La corrección es de diseño: no hay prueba que la
ejercite, porque reproducir la carrera exige los dos núcleos reales.

### 1.2 El reloj compartido se leía y escribía sin protección — **alto**

`syncNTP()` escribe la época base desde el Core 1 (taskRed reintenta NTP en cada
reconexión) y `timestampISO8601()` la lee desde el Core 0 cada 30 s. Eran tres
estáticas sueltas (`time_t`, `unsigned long`, `bool`) sin sincronización. Un
lector podía ver la época nueva junto con la referencia de `millis()` vieja y
emitir un timestamp desplazado por el uptime completo del nodo, justo en el
instante en que se recupera la red.

**Cambio.** El estado se encapsula en `core::Reloj` y los accesos se serializan
con un spinlock (`portMUX_TYPE`). La llamada bloqueante `getLocalTime()` queda
fuera de la sección crítica.

Archivos: `src/payload/PayloadBuilder.cpp`, `src/core/Reloj.{h,cpp}`.

---

## 2. Corrección de los datos

### 2.1 El timestamp retrocede 49 días si NTP no vuelve a sincronizar — **alto**

`_epochBase + (millis() - _millisBase) / 1000` es correcto solo mientras la
distancia entre ambos instantes quepa en 32 bits. `millis()` desborda a los
~49.7 días. Un nodo con más uptime que eso sin resincronizar NTP veía la
diferencia dar la vuelta y el timestamp retrocedía casi dos meses de golpe. El
backend lo rechazaría por `timestamp_demasiado_antiguo` y el nodo dejaría de
registrar sin ningún error visible en el monitor serie.

**Cambio.** `core::Reloj::avanzar()` rebasa la referencia en cada llamada y
conserva los milisegundos sobrantes, así que solo hay que asumir que dos
llamadas consecutivas distan menos de 49 días (distan 30 s).

Prueba: `test_reloj.cpp::test_el_desbordamiento_de_millis_no_retrasa_el_reloj`,
`test_no_se_pierden_los_milisegundos_sobrantes`.

### 2.2 La hora de compilación se convertía con `mktime()`, que es hora local — **medio**

`mktime()` interpreta el `struct tm` como hora **local** y aplica zona horaria y
horario de verano. Se usaba para convertir `__DATE__`/`__TIME__`, que es lo que
se emite como timestamp UTC cuando NTP falla. Además `gmtime()` devuelve un
`struct tm` estático compartido, y se llamaba desde el Core 0 mientras el Core 1
podía estar dentro de NTP.

**Cambio.** Conversión civil↔días calculada explícitamente (algoritmo de
Hinnant), sin estado global ni dependencia del huso configurado.

Prueba: `test_reloj.cpp::test_hora_de_compilacion_se_interpreta_como_utc`,
`test_formato_iso8601`, `test_anio_bisiesto`, `test_dia_de_una_cifra`.

### 2.3 Una apertura de puerta en curso al arrancar no se contabilizaba — **medio**

`MC38Sensor` usaba `_abiertaDesde == 0` como centinela de "puerta cerrada".
`millis()` vale 0 durante el primer milisegundo tras el arranque y vuelve a
valer 0 al desbordar cada ~49.7 días: en ambos instantes una apertura en curso
se daba por cerrada y su duración se perdía.

Lo encontró una prueba del host escrita durante esta revisión
(`test_arranque_con_la_puerta_abierta`), no la lectura del código.

**Cambio.** Bandera explícita `_aperturaEnCurso` en vez del centinela.

Archivos: `src/core/Antirrebote.h`.

### 2.4 Índice de archivo desbordado a los ~34 días de operación — **medio**

Con lecturas cada 30 s el contador llega a 99999 en unos 34 días. `snprintf` con
`"%05d"` empezaba entonces a emitir nombres de 6 dígitos, y el parseo del índice
—`substring(0, 5)`— leía "10000" de "100000.json": un índice ya usado. Se rompían
a la vez el orden FIFO y la unicidad de los nombres, justo en el escenario de
operación prolongada que la tesis quiere demostrar.

**Cambio.** `core::siguienteIndice()` da la vuelta explícitamente a 1;
`core::indiceDeNombre()` parsea hasta el punto y rechaza lo que no encaje;
`core::ordenarFIFO()` ordena por índice numérico y no alfabéticamente, dejando
los nombres irreconocibles al final para que no bloqueen el drenaje.
`saveReading()` libera el archivo previo si el índice dio la vuelta.

Prueba: `test_cola.cpp::test_el_indice_da_la_vuelta_al_agotarse`,
`test_nombres_invalidos_se_rechazan`, `test_orden_fifo_es_cronologico`,
`test_nombres_corruptos_van_al_final`.

---

## 3. Integridad del buffer offline

### 3.1 Un archivo truncado se publicaba como lectura válida — **alto**

Un corte de corriente a mitad de `f.print()` deja un archivo con medio JSON.
`readFile()` solo comprobaba que no estuviera vacío, así que ese medio JSON se
publicaba, el broker lo aceptaba, el backend lo rechazaba con `ValidationError`
y `drenarBuffer()` lo borraba igual. Resultado: lectura perdida en silencio y
ruido en los logs del backend. La documentación (§3.6) afirmaba que "LittleFS
detecta archivo corrupto (CRC falla). Lo descarta", cosa que el código no hacía.

**Cambio.** `core::esPayloadIntegro()` comprueba llaves balanceadas fuera de
cadenas y el límite de tamaño antes de publicar. Es una comprobación
deliberadamente sintáctica: no valida el esquema —eso es trabajo del backend—,
solo descarta lo que se sabe roto. `readFile()` devuelve "" si no pasa, y
`drenarBuffer()` ya descartaba los ilegibles.

Prueba: `test_cola.cpp::test_payload_truncado_se_rechaza`,
`test_payload_con_llaves_desbalanceadas_se_rechaza`,
`test_llaves_dentro_de_cadenas_no_cuentan`.

### 3.2 Escrituras a flash sin comprobar y sin cota de tamaño — **medio**

`saveReading()` solo comprobaba `written == 0`. Una escritura **parcial** (flash
llena, partición dañada) se daba por buena y dejaba un archivo truncado en la
cola. Tampoco se validaba el tamaño del payload antes de gastar un ciclo de
escritura de flash, ni el del archivo antes de cargarlo en RAM al leerlo.

**Cambio.** Se rechaza el payload fuera de `1..LITTLEFS_MAX_FILESIZE` **antes**
de tocar flash; se exige `written == strlen(payload)` y se borra el archivo si no
cuadra; `readFile()` corta por tamaño antes de reservar heap.

### 3.3 Descriptores de archivo sin cerrar al recorrer el directorio — **medio**

`openNextFile()` deja abierto el descriptor anterior. Con 200 archivos en la
cola, cada recorrido acumulaba 200 descriptores. Se cierran explícitamente.

Archivos: `src/storage/LittleFSBuffer.cpp`.

---

## 4. Seguridad

### 4.1 El certificado CA se pasaba a mbedTLS como puntero colgante — **crítico**

`WiFiClientSecure::setCACert()` **no copia** la cadena: guarda el puntero. Se le
pasaba el `c_str()` de un `String` local de `_loadCACertificate()`, que se
destruía al volver. mbedTLS parseaba memoria liberada durante el handshake; que
funcionase dependía de que nadie hubiera reutilizado aún ese trozo de heap. Es
un fallo de seguridad, no solo de estabilidad: el resultado del parseo de la CA
determina si la validación del servidor tiene algún valor.

**Cambio.** El PEM se guarda en un miembro `String` de `MQTTManager`, con vida
igual a la del objeto. El mismo problema afectaba a `host` y `password`, que
ahora vienen de NVS en tiempo de ejecución: `PubSubClient::setServer()` también
guarda punteros, así que se copian a miembros.

### 4.2 Sin certificado CA, el nodo intentaba conectar igual — **alto**

Si `_loadCACertificate()` fallaba, se registraba el error y se seguía adelante.
El firmware nunca llama a `setInsecure()` —eso está bien y se ha dejado
documentado para que no se añada—, pero el comportamiento correcto es fallo
cerrado y explícito.

**Cambio.** `connect()` devuelve `false` sin intentar nada si no hay CA válida o
si el nodo no está aprovisionado. Se valida además que el archivo contenga
realmente un PEM (`-----BEGIN CERTIFICATE-----`) y no el marcador que el CI
escribe. Las lecturas siguen guardándose en LittleFS: el nodo registra la cadena
de frío aunque no pueda publicarla.

Archivos: `src/connectivity/MQTTManager.{h,cpp}`.

### 4.3 Credenciales en texto plano dentro del binario (RNF-05) — **alto**

`config.h` traía valores por defecto compilados: `WIFI_PASSWORD
"cambiar_en_produccion"`, `MQTT_TOKEN "token_generado_en_emqx"`, `MQTT_HOST
"tu-instancia.emqx.cloud"`. Aunque fueran marcadores, el efecto era doble: el
nodo arrancaba con credenciales inválidas y fallaba en el broker con un error
genérico indistinguible de un token caducado o de una regla ACL mal puesta; y el
patrón invitaba a escribir las credenciales reales en ese archivo y subirlo.

**Cambio.** Las macros quedan vacías. Las credenciales se cargan en tiempo de
ejecución con precedencia **NVS > build_flags > nada**. NVS permite aprovisionar
cada nodo sin recompilar y sin que el token esté en ningún archivo del
repositorio. Si el nodo no está aprovisionado, lo dice en el arranque y no
intenta conectar, en vez de entrar en un ciclo de reintentos opaco. El log nunca
imprime los valores, solo su origen.

Archivos: `src/config.h`, `src/system/Credenciales.{h,cpp}`,
`src/core/Credenciales.{h,cpp}`, `src/connectivity/WiFiManager.{h,cpp}`,
`src/main.cpp`.

Prueba: `test_credenciales.cpp` (5 casos), incluido que los marcadores concretos
que este repositorio tuvo compilados se traten como "no aprovisionado".

*Sin validar en hardware.* La lectura de NVS con `Preferences` compila pero no
se ha ejercitado en un ESP32; el camino de reserva por `build_flags` sí es el que
usa el CI.

### 4.4 Desbordamiento de pila al parsear `__DATE__` — **bajo**

`sscanf(__DATE__, "%s %d %d", month, ...)` sobre `char month[4]`, sin acotar el
ancho. `__DATE__` siempre mide lo mismo, así que no era explotable en la
práctica, pero es un `%s` sin cota sobre un buffer de pila.

**Cambio.** `%15s` sobre `char nombreMes[16]`, con validación de rangos de mes,
día y hora.

Prueba: `test_reloj.cpp::test_fecha_de_compilacion_invalida_devuelve_cero`.

---

## 5. Watchdog

### 5.1 El TWDT no vigilaba ninguna de las dos tareas reales — **alto**

`esp_task_wdt_add()` no se llamaba en ninguna parte. El único vigilado era
`loopTask`, la tarea de Arduino, que en este firmware solo hace `delay(1000)`:
se alimentaba siempre y por tanto nunca habría disparado. Si `taskRed` se
colgaba dentro del handshake TLS o `taskSensores` en el bus 1-Wire, el nodo se
quedaba mudo indefinidamente y el único síntoma era el LWT del broker 60 s
después. El código anterior ya reconocía esto en un comentario y lo dejaba
pendiente por el riesgo de un ciclo de reinicios.

**Cambio.** `src/system/Watchdog.{h,cpp}` reconfigura el TWDT y suscribe ambas
tareas al arrancar. `alimentarWatchdog()` se siembra en los bucles de sondeo: la
ventana de 30 s del Core 0, el sondeo de conversión del DS18B20 (hasta 2 s), el
bucle de asociación Wi-Fi (hasta 15 s) y entre cada etapa bloqueante de
`taskRed`.

### 5.2 De dónde sale el plazo de 30 s — **acotado en 2.ª ronda**

En la primera ronda los 30 s eran un número elegido a ojo, y así se dijo. Ahora
se derivan del mayor tramo que puede ejecutarse **sin poder alimentar** al
watchdog, es decir, del bloqueo más largo dentro de una sola llamada al SDK que
no devuelve el control hasta terminar.

Inventario de bloqueos de `taskRed`, que es el peor de los dos núcleos:

| Tramo | Cota | ¿Alimentable? |
|---|---|---|
| Asociación Wi-Fi (`WiFi.begin` + espera) | 15 000 ms | Sí, cada 500 ms |
| Handshake TLS (`WiFiClientSecure`) | 10 000 ms | **No** |
| CONNACK del broker (command timeout) | 5 000 ms | **No** |
| PUBACK por lectura (command timeout) | 5 000 ms | **No** |
| `getLocalTime()` de NTP | 10 000 ms | **No** |
| `mqtt.loop()` | < 100 ms | — |

El peor tramo no alimentable es `mqtt.connect()`: el handshake TLS y la espera
del CONNACK ocurren dentro de la misma llamada, sin punto intermedio donde
intercalar un `esp_task_wdt_reset()`. Son **10 000 + 5 000 = 15 000 ms**. NTP
(10 s) y cada PUBACK (5 s) quedan por debajo, y entre ellos sí se alimenta.

En `taskSensores` el peor tramo no alimentable es una transacción I2C del SHT31
(~15 ms). El sondeo del DS18B20 llega a 2 000 ms pero alimenta cada 10 ms. La
espera por el mutex del buffer está acotada por una operación de flash (decenas
de ms), porque el mutex nunca se mantiene durante una publicación MQTT.

**Plazo = 2 × 15 000 ms = 30 s.** El factor 2 cubre la variabilidad de
planificación entre dos núcleos con Wi-Fi activo, no un tramo adicional. El
valor coincide con el de la primera ronda, pero ahora está justificado y, sobre
todo, **se calcula solo**: `WATCHDOG_TIMEOUT_S` se deriva en tiempo de
compilación de `TLS_HANDSHAKE_TIMEOUT_MS` y `MQTT_COMMAND_TIMEOUT_MS`, así que
subir cualquiera de los dos sube el plazo automáticamente.

Dos `static_assert` fijan las hipótesis del análisis para que un cambio de
constantes no las invalide en silencio:

1. `PEOR_BLOQUEO_NO_ALIMENTABLE_MS >= NTP_SYNC_TIMEOUT_MS` — si alguien sube el
   timeout de NTP por encima del de conexión, NTP pasa a ser el peor tramo y la
   tabla deja de ser válida.
2. `WATCHDOG_TIMEOUT_S <= MQTT_KEEPALIVE_SEC` — si el plazo superara el
   keep-alive, EMQX declararía el nodo muerto y publicaría su LWT **antes** de
   que el watchdog llegara a reiniciarlo: el nodo aparecería como caído sin
   recuperarse, que es el peor de los dos comportamientos.

Ambas se comprobaron violándolas a propósito: el build falla con el mensaje
correspondiente (salida en §11).

*Sin validar en hardware.* Las cotas son las configuradas en el código, no
tiempos medidos en campo. Lo que el análisis garantiza es que el plazo es
coherente con los timeouts que el propio firmware impone; lo que no puede
garantizar es que el SDK los respete siempre.

### 5.3 Rama IDF 5 / Arduino core 3.x: retirada — **resuelto en 2.ª ronda**

La primera ronda dejó escrita una rama `#if ESP_IDF_VERSION_MAJOR >= 5` para la
API nueva del TWDT que **nunca se compiló**, porque el toolchain resuelve a
IDF 4. Código muerto no verificado en un firmware es peor que no tenerlo:
aparenta soportar algo que nadie ha comprobado.

**Cambio.** La rama se retiró. En su lugar queda un `#error` que convierte una
futura actualización del core en un fallo de compilación explícito, con la
instrucción de qué hay que reescribir y la exigencia de verificarlo. Combinado
con la versión de plataforma ya fijada (§6.5), el firmware no puede acabar
compilando contra una API del watchdog que nadie ha probado.

---

## 6. Conectividad

### 6.1 Reconexión Wi-Fi sin `disconnect()` previo — **medio**

Llamar a `WiFi.begin()` sobre una sesión a medio establecer deja al supplicant
en un estado desde el que no reintenta; el nodo se quedaba en `WL_DISCONNECTED`
hasta el reinicio. Se añade `WiFi.disconnect(false, false)` antes de cada
intento.

### 6.2 El backoff no sobrevivía al desbordamiento de `millis()` — **medio**

La comparación era correcta por usar aritmética sin signo, pero no había nada
que lo fijara y el valor base estaba duplicado (`WIFI_RECONNECT_BASE_MS` en
`config.h` y un `1000` literal en `WiFiManager.h`), sin garantía de que
coincidieran.

**Cambio.** La política vive en `core::Backoff`, con el valor base tomado de
`config.h` y cubierta por pruebas, incluido el caso del desbordamiento.

Prueba: `test_backoff.cpp` (6 casos).

### 6.3 El evento `online` no comprobaba si se publicó — **bajo**

`_mqtt.publish(TOPIC_LWT, LWT_PAYLOAD_ONLINE, false)` ignoraba el retorno. Si
falla, el backend no marca el nodo como conectado hasta la primera lectura. No
es fatal, pero debe quedar registrado.

### 6.4 QoS 1 en publicación: HU-11 no se cumplía — **crítico, corregido en 2.ª ronda**

`PubSubClient::publish()` publica **siempre** en QoS 0. La librería no ofrece
QoS 1 al publicar: no hay PUBACK, ni packetId, ni callback de confirmación. El
valor devuelto significa "el paquete se escribió en el socket TLS", no "el
broker lo recibió". La entrega nodo→broker era por tanto *at-most-once*, y HU-11
figuraba como implementada en la documentación de la tesis sin estarlo.

La mitigación de la primera ronda —revalidar la sesión antes de borrar el
archivo— acotaba la ventana pero no la cerraba: si la sesión caía con el paquete
en vuelo, esa lectura se perdía y el archivo ya se había borrado.

**Evaluación de alternativas.**

| Opción | QoS 1 real | Coste del cambio | Veredicto |
|---|---|---|---|
| `knolleary/PubSubClient` (actual) | No | — | No cumple HU-11 |
| `256dpi/MQTT` (arduino-mqtt, sobre lwmqtt) | Sí, con PUBACK y verificación de packetId | Bajo: misma forma síncrona, acepta cualquier `Client&` | **Elegida** |
| `marvinroger/AsyncMqttClient` | Sí | Alto: modelo por eventos, reescribir `taskRed` y la lógica de drenaje como callbacks | Descartada |
| `esp-mqtt` (nativa de ESP-IDF) | Sí | Alto: sale del framework Arduino, obliga a reescribir la capa TLS | Descartada |

Se eligió `256dpi/MQTT` porque es la única que resuelve el requisito **sin
tocar nada más**: conserva la forma `begin/connect/loop/publish` del código
existente y acepta cualquier `Client&`, de modo que la capa TLS queda intacta
—el mismo `WiFiClientSecure` con el mismo `setCACert()`— y con ella la
validación de CA, el SNI y la autenticación por `device_id`. Migrar a
`AsyncMqttClient` a semanas de la defensa habría obligado a reescribir el bucle
de red completo, que es justo el código que no se puede probar sin hardware.

**Verificación de que el QoS 1 es real** (leída en el código de la librería, no
supuesta): `MQTTClient::publish()` delega en `lwmqtt_publish()`, que tras enviar
el PUBLISH espera el paquete PUBACK dentro del *command timeout*, comprueba que
el tipo de paquete sea el esperado y que el `packet_id` recibido coincida con el
emitido. Solo entonces devuelve `LWMQTT_SUCCESS` y `publish()` devuelve `true`.

**Cambio.**
- `MQTTManager::publicarLectura()` publica con `qos=1` y devuelve un
  `core::ResultadoPublicacion` de tres estados: `Confirmado` (PUBACK recibido),
  `Fallo` (sin confirmación, sesión viva) y `SinConexion`.
- El archivo del buffer se borra **solo** con `Confirmado`.
- El LWT sigue registrándose en el CONNECT con QoS 1; el evento `online` pasa
  también a QoS 1 y se comprueba su confirmación.
- Los diagnósticos de conexión distinguen ahora `lastError()` (transporte y
  protocolo) de `returnCode()` (rechazo del broker), que PubSubClient mezclaba
  en un único número.

**Contrapartida asumida.** `publish()` ahora **bloquea** hasta el PUBACK o hasta
`MQTT_COMMAND_TIMEOUT_MS` (5 s). Dos consecuencias, ambas tratadas:
1. Entra en el presupuesto del watchdog (§5.1).
2. El mutex del buffer **no** puede mantenerse durante la publicación, o el
   Core 0 esperaría hasta 5 s para guardar su lectura de los 30 s.
   `drenarBuffer()` lee bajo mutex, publica fuera y borra bajo mutex otra vez.

Archivos: `src/connectivity/MQTTManager.{h,cpp}`, `src/core/ColaFIFO.{h,cpp}`,
`src/main.cpp`, `src/config.h`, `platformio.ini`.

Prueba: `test_cola_fifo.cpp::test_no_se_borra_nada_sin_puback_confirmado`,
`test_el_drenaje_se_detiene_al_primer_fallo_y_conserva_el_resto`,
`test_tras_un_fallo_se_reintenta_la_misma_lectura`.

*Sin validar en hardware.* Que el PUBACK llegue de EMQX Cloud sobre TLS dentro
de los 5 s configurados no se ha medido: es la primera cosa que hay que
comprobar sobre hardware real, porque un command timeout demasiado ajustado
haría que el nodo reintentara indefinidamente lecturas que el broker ya recibió.
La deduplicación del backend (`UNIQUE(device_id, timestamp)`) hace que ese caso
degrade en tráfico redundante, no en datos corruptos.

### 6.5 Versión de plataforma sin fijar — **resuelto en 2.ª ronda**

`platform = espressif32` sin versión resuelve a la última publicada. Dos
compilaciones en fechas distintas podían usar cores de Arduino distintos: el
binario que se defiende no sería necesariamente el que se compiló al escribir la
memoria, y una actualización del core podía cambiar la API del TWDT bajo los
pies del proyecto.

**Cambio.** `platform = espressif32@7.0.1`, que es la versión con la que se
compila y se verifica hoy: Arduino core 2.0.17 sobre ESP-IDF 4.4, la rama del
TWDT implementada en `system/Watchdog.cpp`.

---

## 7. Memoria y payload

### 7.1 `JsonDocument` en el heap por cada lectura — **medio**

El payload son ocho campos planos de tipo fijo. Construir un `JsonDocument` de
ArduinoJson cada 30 s, serializarlo a un `String` y devolverlo por valor
fragmenta el heap de un proceso que corre durante semanas, y ataba el contrato
con el backend a una dependencia que impedía compilarlo fuera del ESP32.

**Cambio.** `core::serializarLectura()` construye el JSON con `std::string` y
`reserve()`, con dos decimales fijos (de sobra para ±0.2 °C del SHT31 y
0.0625 °C del DS18B20), lo que además hace el tamaño del payload predecible y
permite garantizar el techo de 512 bytes por construcción. `ArduinoJson` sale de
`lib_deps`. `PayloadBuilder` queda como envoltorio Arduino.

Efecto medido en el binario: Flash 78.3 % → 78.9 %, RAM 14.3 % (sin cambio). El
crecimiento viene del código añadido (watchdog, NVS, mutex, validación de
integridad), no de la serialización.

### 7.2 `device_id` sin escapar — **bajo**

Un `device_id` o `firmware_version` con comillas —posible desde un `build_flag`
mal escrito— rompía el JSON y el backend descartaba el mensaje sin explicación
útil. `core::escaparJSON()` los sanea.

Prueba: `test_payload.cpp::test_comillas_en_device_id_se_escapan`.

### 7.3 `#include <SPIFFS.h>` innecesario — **bajo**

Arrastraba al binario el driver de un sistema de archivos que este firmware no
monta. Eliminado.

### 7.4 Las cadenas de formato de `LOG_I` nunca se compilaban — **medio**

`LOG_I` se expande a `((void)0)` salvo con `-DDEBUG_IOT`, que no estaba en
`build_flags`. El compilador nunca verificaba sus cadenas de formato: había
varios `%d` recibiendo `size_t`, y solo se habrían manifestado al depurar sobre
hardware. Corregidos, y el CI añade una compilación con `DEBUG_IOT` activo para
que no vuelva a pasar.

---

## 8. Rangos de sensores

Los tres drivers ya devolvían `NAN` ante un fallo, no `0.0`: **no existía aquí el
equivalente al defecto B-05 del backend**. Lo que sí había era duplicación de los
umbrales entre cada `.h` y su `.cpp`, sin nada que garantizara que coincidían, y
ninguna prueba que fijara el contrato.

**Cambio.** Umbrales unificados en `core/RangosSensores.h` y cubiertos por
pruebas, incluidos dos casos que documentan decisiones fáciles de revertir por
error:

- Los 85 °C del power-on reset del DS18B20 **no** se filtran: son físicamente
  posibles y descartarlos ocultaría un fallo de cableado real.
- El rango operativo de la cadena de frío (2-8 °C) **no** se filtra aquí:
  justamente las excursiones son lo que hay que detectar.

Prueba: `test_sensores.cpp` (7 casos).

---

## 9. Pruebas y CI

`src/core/` contiene ahora la lógica del firmware que no depende de Arduino:
serialización del payload, reglas de la cola FIFO, antirrebote, backoff, rangos
de sensores, reloj y validación de credenciales. Es lógica pura con el tiempo
inyectado; los drivers y managers quedan como capa fina sobre el SDK.

`[env:native]` en `platformio.ini` compila **solo** `src/core/` con
`build_src_filter = +<core/>`. Si alguien mete una dependencia del SDK ahí
dentro, el entorno deja de compilar: es la barrera que se busca.

| Archivo de prueba | Cubre | Casos |
|---|---|---|
| `test_payload.cpp` | Contrato con el backend, null vs 0.0, techo de 512 B, escapado, bajo cero, redondeo | 14 |
| `test_cola.cpp` | Nombres, vuelta del índice, orden FIFO, integridad tras corte | 11 |
| `test_antirrebote.cpp` | Rebotes, aperturas entre reportes, ventana, desbordamiento | 8 |
| `test_backoff.cpp` | Secuencia 1s→60s, tope, reinicio, desbordamiento | 6 |
| `test_sensores.cpp` | Rangos, código de error -127 °C, no filtrar excursiones | 7 |
| `test_reloj.cpp` | ISO 8601, bisiestos, hora de compilación, desbordamiento | 9 |
| `test_credenciales.cpp` | Marcadores, vacíos, aprovisionamiento completo | 5 |
| `test_cola_fifo.cpp` | Intercalado de los dos cores, ABA de índices, borrado solo tras PUBACK, cortes de corriente, saturación | 16 |

### 9.1 Pruebas de concurrencia: qué demuestran y qué no

La primera ronda dejó las correcciones de concurrencia sin ninguna prueba. La
carrera real entre dos núcleos no se puede reproducir en el host —no hay dos
schedulers—, pero sí se puede reproducir el **orden de operaciones** que ambos
núcleos ejecutan sobre la cola, intercalado a mano en un solo hilo.

Para ello el protocolo del buffer se extrajo a `core::ColaFIFO`, que opera sobre
una interfaz `AlmacenLecturas`. `LittleFSBuffer` la implementa sobre LittleFS y
añade el mutex; las pruebas la implementan sobre un mapa en memoria capaz de
simular escrituras parciales, flash llena y archivos truncados.

Intercalados cubiertos:

- El Core 0 escribe mientras el Core 1 está a mitad del drenaje: nada se pierde
  ni se adelanta a una lectura más antigua.
- El Core 1 encuentra que un archivo de su instantánea ya no existe, porque el
  FIFO del Core 0 lo descartó por saturación.
- El índice no se reutiliza tras vaciarse la cola (riesgo ABA: un nombre de una
  instantánea antigua que pasa a referirse a otra lectura).
- Un reinicio siembra el índice desde disco y no reutiliza nombres pendientes.
- La vuelta del índice a los 99999 archivos libera el archivo previo.
- Un archivo truncado se descarta sin bloquear a las lecturas posteriores; una
  cola entera de basura se purga en vez de atascarse.
- Nada se borra sin PUBACK confirmado; tras un fallo se reintenta la misma
  lectura.

**Lo que estas pruebas NO demuestran**, y conviene decirlo en la defensa: que el
mutex de `LittleFSBuffer` esté correctamente colocado, y que el planificador de
FreeRTOS produzca solo los intercalados considerados. Demuestran que el
algoritmo es correcto bajo cualquier intercalado que el planificador pueda
producir, que es la mitad del problema y la única mitad verificable sin
hardware. La otra mitad sigue dependiendo de revisión manual.

El CI pasa a tener dos jobs: `pruebas-host` (`pio test -e native`) y `compilar`,
este último con una segunda pasada con `-DDEBUG_IOT` para que las cadenas de
formato de los logs se verifiquen.

---

## 10. Lo que queda pendiente

Tras la segunda ronda, la lista se reduce a esto:

1. **Validación sobre hardware real.** Sigue siendo lo único importante que
   falta. Nada se ha ejecutado en un ESP32: sin sensores, sin broker, sin flash.
   Compilar no es ejecutar.
2. **Medir el PUBACK real contra EMQX Cloud.** `MQTT_COMMAND_TIMEOUT_MS` está en
   5 s por análisis, no por medición. Si el RTT real lo supera, el nodo
   reintentaría lecturas ya entregadas; degrada en tráfico redundante, no en
   datos corruptos, porque el backend deduplica.
3. **Confirmar el plazo del watchdog en campo.** El presupuesto de §5.2 usa las
   cotas que el firmware configura, no tiempos observados.
4. **El mutex del buffer no tiene prueba automatizada.** Ver §9.1.
5. **Corregir la documentación de la tesis.** `IoT-documentacion_iot.md` §3.6 y
   §3.8 describen HU-11 como no cumplida y la publicación como QoS 0; §8.2 la
   lista como mejora futura. Ya no es cierto: la publicación es QoS 1 con PUBACK
   verificado. Ese documento está fuera de este repositorio y **no se ha
   tocado** — hay que actualizarlo antes de la defensa.

## 11. Verificación ejecutada

PlatformIO Core 6.1.19 en un venv local (`.piovenv/`, ignorado por git); entorno
global intacto. Plataforma fijada a `espressif32@7.0.1` (Arduino core 2.0.17,
ESP-IDF 4.4).

```
$ pio test -e native
----------------- native:test_core [PASSED] Took 0.93 seconds -----------------

=================================== SUMMARY ===================================
Environment    Test       Status    Duration
-------------  ---------  --------  ------------
native         test_core  PASSED    00:00:00.932
================= 76 test cases: 76 succeeded in 00:00:00.932 =================

$ pio run -e esp32dev
RAM:   [=         ]  14.4% (used 47084 bytes from 327680 bytes)
Flash: [========  ]  79.3% (used 987761 bytes from 1245184 bytes)
========================= [SUCCESS] Took 6.64 seconds =========================

$ pio run -e esp32dev --target buildfs
/certs/README.md
/certs/root_ca.pem
========================= [SUCCESS] Took 0.73 seconds =========================

$ PLATFORMIO_BUILD_FLAGS=-DDEBUG_IOT pio run -e esp32dev
========================= [SUCCESS] Took 8.05 seconds =========================
```

Evolución del binario a lo largo de las dos rondas:

| | RAM | Flash |
|---|---|---|
| Estado inicial | 14.3 % (46 848 B) | 78.3 % |
| Tras 1.ª ronda | 14.3 % (46 848 B) | 78.9 % (982 969 B) |
| Tras 2.ª ronda (QoS 1 + lwmqtt) | 14.4 % (47 084 B) | 79.3 % (987 761 B) |

El cambio de PubSubClient a lwmqtt cuesta 4.8 KB de flash y 236 B de RAM. Es el
precio de la garantía de entrega de HU-11.

### Pruebas negativas de los `static_assert` del watchdog

Se comprobó que no son decorativos violándolos a propósito:

```
$ sed -i '' 's/NTP_SYNC_TIMEOUT_MS     10000/NTP_SYNC_TIMEOUT_MS     20000/' src/config.h
$ pio run -e esp32dev
src/system/Watchdog.h:71:46: error: static assertion failed: NTP pasó a ser el
bloqueo no alimentable más largo: rehacer el presupuesto del watchdog en
system/Watchdog.h.

$ sed -i '' 's/MQTT_KEEPALIVE_SEC     60/MQTT_KEEPALIVE_SEC     20/' src/config.h
$ pio run -e esp32dev
src/system/Watchdog.h:79:34: error: static assertion failed: El plazo del
watchdog supera el keep-alive MQTT: el broker declararía el nodo muerto antes de
que el watchdog lo reinicie.
```

(`src/config.h` restaurado tras ambas pruebas.)

### Lo que NO se ha verificado

- **Nada sobre hardware.** Sin ESP32, sin sensores, sin EMQX. Toda afirmación
  sobre comportamiento en ejecución es análisis del código, no observación.
- **El QoS 1 contra un broker real.** Que `lwmqtt` espera y valida el PUBACK se
  verificó leyendo el código de la librería (`lwmqtt/client.c`), no observando
  un intercambio real con EMQX.
- **El mutex del buffer.** Ver §9.1.
- **La rama IDF 5 del watchdog** ya no existe: se retiró en vez de dejarla sin
  compilar (§5.3).
