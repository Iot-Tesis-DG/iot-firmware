/**
 * Contrato del payload de lectura (HU-05, §3.5 de la documentación IoT).
 *
 * La prueba clave es `test_payload_coincide_con_el_documentado`: compara la
 * salida literal contra el JSON publicado en la documentación y validado por
 * `LecturaPayload` en el backend. Ese contrato ya se rompió una vez sin que
 * nadie lo notara —el firmware emitía `duracion_apertura_segundos` y el backend
 * no lo declaraba, con `extra="forbid"`, así que rechazaba el 100 % de los
 * mensajes— y no había ninguna prueba del lado del firmware que lo fijara.
 */
#include <unity.h>

#include <cmath>
#include <string>

#include "core/PayloadCore.h"

static core::Lectura lecturaBase() {
    core::Lectura l;
    l.deviceId = "FARM-01-CDL";
    l.firmwareVersion = "1.0.0";
    l.timestamp = "2026-07-25T12:34:56Z";
    l.online = true;
    l.temperaturaInterna = 4.5f;
    l.temperaturaAmbiental = 5.2f;
    l.humedadAmbiental = 62.0f;
    l.aperturaRefrigerador = false;
    l.duracionAperturaSegundos = 0;
    return l;
}

void test_payload_coincide_con_el_documentado(void) {
    const std::string json = core::serializarLectura(lecturaBase());
    const std::string esperado =
        "{\"device_id\":\"FARM-01-CDL\","
        "\"timestamp\":\"2026-07-25T12:34:56Z\","
        "\"estado_conectividad\":\"online\","
        "\"firmware_version\":\"1.0.0\","
        "\"temperatura_interna\":4.50,"
        "\"temperatura_ambiental\":5.20,"
        "\"humedad_ambiental\":62.00,"
        "\"apertura_refrigerador\":false,"
        "\"duracion_apertura_segundos\":0}";
    TEST_ASSERT_EQUAL_STRING(esperado.c_str(), json.c_str());
}

/// El fallo equivalente al B-05 del backend: una lectura fallida NO puede
/// salir como 0.0, porque 0.0 °C en una cadena de frío de 2-8 °C es una
/// excursión crítica indistinguible de un sensor desconectado.
void test_sensor_fallido_sale_como_null_no_como_cero(void) {
    core::Lectura l = lecturaBase();
    l.temperaturaInterna = NAN;
    l.temperaturaAmbiental = NAN;
    l.humedadAmbiental = NAN;

    const std::string json = core::serializarLectura(l);
    TEST_ASSERT_TRUE(json.find("\"temperatura_interna\":null") != std::string::npos);
    TEST_ASSERT_TRUE(json.find("\"temperatura_ambiental\":null") != std::string::npos);
    TEST_ASSERT_TRUE(json.find("\"humedad_ambiental\":null") != std::string::npos);
    TEST_ASSERT_TRUE(json.find(":0.00") == std::string::npos);
    TEST_ASSERT_TRUE(json.find("nan") == std::string::npos);
    TEST_ASSERT_TRUE(json.find("NaN") == std::string::npos);
}

/// Los campos de sensor se emiten SIEMPRE, nunca se omiten (HU-05 esc. 2).
void test_campos_de_sensor_nunca_se_omiten(void) {
    core::Lectura l = lecturaBase();
    l.temperaturaInterna = NAN;
    const std::string json = core::serializarLectura(l);
    TEST_ASSERT_TRUE(json.find("\"temperatura_interna\"") != std::string::npos);
}

/// `allow_inf_nan=False` en el backend: un infinito serializado como número
/// haría fallar la validación y se perdería la lectura entera.
void test_infinito_sale_como_null(void) {
    core::Lectura l = lecturaBase();
    l.temperaturaInterna = INFINITY;
    l.temperaturaAmbiental = -INFINITY;
    const std::string json = core::serializarLectura(l);
    TEST_ASSERT_TRUE(json.find("\"temperatura_interna\":null") != std::string::npos);
    TEST_ASSERT_TRUE(json.find("\"temperatura_ambiental\":null") != std::string::npos);
    TEST_ASSERT_TRUE(json.find("inf") == std::string::npos);
}

void test_estado_offline(void) {
    core::Lectura l = lecturaBase();
    l.online = false;
    const std::string json = core::serializarLectura(l);
    TEST_ASSERT_TRUE(json.find("\"estado_conectividad\":\"offline\"") != std::string::npos);
}

void test_apertura_de_puerta_con_duracion(void) {
    core::Lectura l = lecturaBase();
    l.aperturaRefrigerador = true;
    l.duracionAperturaSegundos = 18;
    const std::string json = core::serializarLectura(l);
    TEST_ASSERT_TRUE(json.find("\"apertura_refrigerador\":true") != std::string::npos);
    TEST_ASSERT_TRUE(json.find("\"duracion_apertura_segundos\":18") != std::string::npos);
}

/// El techo de 512 bytes es un requisito, no una recomendación: publicar un
/// payload truncado es peor que saltarse un ciclo de 30 s.
void test_payload_que_excede_el_maximo_se_descarta(void) {
    core::Lectura l = lecturaBase();
    l.deviceId = std::string(600, 'X');
    TEST_ASSERT_TRUE(core::serializarLectura(l, 512).empty());
}

void test_payload_normal_cabe_de_sobra_en_512(void) {
    const std::string json = core::serializarLectura(lecturaBase(), 512);
    TEST_ASSERT_FALSE(json.empty());
    TEST_ASSERT_LESS_THAN(512u, (unsigned)json.size());
}

/// Un `device_id` con comillas —posible desde un `build_flag` mal escrito—
/// rompería el JSON y el backend descartaría el mensaje sin explicación útil.
void test_comillas_en_device_id_se_escapan(void) {
    core::Lectura l = lecturaBase();
    l.deviceId = "FARM\"01";
    const std::string json = core::serializarLectura(l);
    TEST_ASSERT_TRUE(json.find("FARM\\\"01") != std::string::npos);
}

/// Las temperaturas bajo cero son el caso NORMAL de una cadena de frío mal
/// regulada (un congelador que congela vacunas que no deben congelarse). El
/// signo no puede perderse en el formateo.
void test_temperaturas_bajo_cero_se_serializan_con_signo(void) {
    core::Lectura l = lecturaBase();
    l.temperaturaInterna = -8.5f;
    l.temperaturaAmbiental = -0.25f;
    const std::string json = core::serializarLectura(l);
    TEST_ASSERT_TRUE(json.find("\"temperatura_interna\":-8.50") != std::string::npos);
    TEST_ASSERT_TRUE(json.find("\"temperatura_ambiental\":-0.25") != std::string::npos);
}

/// Los extremos del rango de la cadena de frío (2-8 °C) y el redondeo a dos
/// decimales no pueden desplazar una lectura al otro lado del umbral.
void test_redondeo_no_cruza_los_umbrales_de_la_cadena_de_frio(void) {
    core::Lectura l = lecturaBase();
    l.temperaturaInterna = 8.004f;
    TEST_ASSERT_TRUE(core::serializarLectura(l).find(":8.00") != std::string::npos);

    l.temperaturaInterna = 1.996f;
    TEST_ASSERT_TRUE(core::serializarLectura(l).find(":2.00") != std::string::npos);
}

/// Un carácter de control sin escapar produce un JSON inválido que el backend
/// descarta sin explicación útil.
void test_caracteres_de_control_se_escapan(void) {
    core::Lectura l = lecturaBase();
    l.deviceId = std::string("FARM\x01" "01");
    const std::string json = core::serializarLectura(l);
    TEST_ASSERT_TRUE(json.find("\\u0001") != std::string::npos);
}

void test_escapado_de_barra_invertida_y_saltos(void) {
    TEST_ASSERT_EQUAL_STRING("a\\\\b", core::escaparJSON("a\\b").c_str());
    TEST_ASSERT_EQUAL_STRING("a\\nb", core::escaparJSON("a\nb").c_str());
    TEST_ASSERT_EQUAL_STRING("a\\tb", core::escaparJSON("a\tb").c_str());
}

/// Duraciones largas de apertura (puerta olvidada abierta toda la noche) no
/// pueden desbordar ni salir con signo.
void test_duracion_de_apertura_muy_larga(void) {
    core::Lectura l = lecturaBase();
    l.aperturaRefrigerador = true;
    l.duracionAperturaSegundos = 4000000000u;  // > INT32_MAX
    const std::string json = core::serializarLectura(l);
    TEST_ASSERT_TRUE(json.find("\"duracion_apertura_segundos\":4000000000") != std::string::npos);
}

void run_tests_payload(void) {
    RUN_TEST(test_temperaturas_bajo_cero_se_serializan_con_signo);
    RUN_TEST(test_redondeo_no_cruza_los_umbrales_de_la_cadena_de_frio);
    RUN_TEST(test_caracteres_de_control_se_escapan);
    RUN_TEST(test_escapado_de_barra_invertida_y_saltos);
    RUN_TEST(test_duracion_de_apertura_muy_larga);
    RUN_TEST(test_payload_coincide_con_el_documentado);
    RUN_TEST(test_sensor_fallido_sale_como_null_no_como_cero);
    RUN_TEST(test_campos_de_sensor_nunca_se_omiten);
    RUN_TEST(test_infinito_sale_como_null);
    RUN_TEST(test_estado_offline);
    RUN_TEST(test_apertura_de_puerta_con_duracion);
    RUN_TEST(test_payload_que_excede_el_maximo_se_descarta);
    RUN_TEST(test_payload_normal_cabe_de_sobra_en_512);
    RUN_TEST(test_comillas_en_device_id_se_escapan);
}
