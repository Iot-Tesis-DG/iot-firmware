# Certificados TLS para el ESP32

## Qué va aquí

Colocar el certificado de la Autoridad Certificadora (CA) raíz que emitió el
certificado TLS del servidor EMQX Cloud. Para EMQX Cloud Serverless esto suele
ser el certificado de **Let's Encrypt** (ISRG Root X1 o X2).

## Cómo obtener el certificado CA raíz

### Opción A — Let's Encrypt (recomendado para EMQX Cloud Serverless)

```bash
# Descargar ISRG Root X1 (válido hasta 2035)
curl -o root_ca.pem https://letsencrypt.org/certs/isrgrootx1.pem

# Opcional: también descargar ISRG Root X2 (ECDSA, respaldo para 2035+)
curl -o root_ca_x2.pem https://letsencrypt.org/certs/isrgrootx2.pem
```

### Opción B — Extraer del propio servidor EMQX

```bash
openssl s_client -connect tu-instancia.emqx.cloud:8883 -showcerts </dev/null 2>/dev/null | \
  awk '/BEGIN CERTIFICATE/,/END CERTIFICATE/' | \
  tail -n +1 | head -n -1 > root_ca.pem
```

Esto extrae TODA la cadena. Quedarse solo con el último certificado (el raíz).

### Opción C — Usar el bundle de Mozilla

```bash
curl -o root_ca.pem https://curl.se/ca/cacert.pem
```

## Flashear a LittleFS

Una vez obtenido `root_ca.pem`, colocarlo en `data/certs/root_ca.pem` y flashear:

```bash
# PlatformIO
pio run --target uploadfs

# O desde el menú de PlatformIO:
# Platform → Upload Filesystem Image
```

## Verificar que funciona

Al arrancar, el monitor serie debe mostrar:

```
[MQTT] Certificado CA cargado (1934 bytes).
[MQTT] Conectando a tu-instancia.emqx.cloud:8883 como 'FARM-01-CDL'...
[MQTT] Conectado a EMQX. Publicando LWT 'online'...
```

Si el certificado no se encuentra:

```
[MQTT] ERROR: No se pudo abrir el certificado CA: /certs/root_ca.pem
[MQTT] ERROR: Ejecutar: 'pio run --target uploadfs' para flashear data/certs/
```
