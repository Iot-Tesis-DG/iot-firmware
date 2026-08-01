/**
 * Validación de rango de sensores (HU-01, HU-02, HU-03).
 *
 * La regla que fijan estas pruebas es una sola: una lectura no confiable sale
 * como NaN, jamás como número. Es el equivalente edge del defecto B-05 del
 * backend — devolver 0.0 ante un fallo de lectura hace que un sensor
 * desconectado sea indistinguible de 0.0 °C reales, que en un rango operativo
 * de 2-8 °C es precisamente una excursión crítica.
 */
#include <unity.h>

#include <cmath>

#include "core/RangosSensores.h"

void test_ds18b20_lectura_valida_pasa(void) {
    TEST_ASSERT_EQUAL_FLOAT(4.5f, core::validarDS18B20(4.5f));
    TEST_ASSERT_EQUAL_FLOAT(-55.0f, core::validarDS18B20(-55.0f));
    TEST_ASSERT_EQUAL_FLOAT(125.0f, core::validarDS18B20(125.0f));
}

/// -127 °C es el código de error de DallasTemperature: sensor ausente, bus en
/// corto o pull-up de 4.7 kΩ faltante.
void test_ds18b20_codigo_de_error_es_nan(void) {
    TEST_ASSERT_TRUE(std::isnan(core::validarDS18B20(-127.0f)));
    TEST_ASSERT_TRUE(core::esFalloDS18B20(-127.0f));
    TEST_ASSERT_TRUE(core::esFalloDS18B20(NAN));
    TEST_ASSERT_TRUE(core::esFalloDS18B20(INFINITY));
}

void test_ds18b20_fuera_de_rango_es_nan(void) {
    TEST_ASSERT_TRUE(std::isnan(core::validarDS18B20(126.0f)));
    TEST_ASSERT_TRUE(std::isnan(core::validarDS18B20(-60.0f)));
}

/// 85 °C es el valor de power-on reset del DS18B20 y a la vez una temperatura
/// físicamente posible. NO se filtra a propósito: descartarlo aquí ocultaría un
/// fallo de cableado real, y el backend tiene más contexto para decidir.
void test_ds18b20_deja_pasar_los_85_grados(void) {
    TEST_ASSERT_EQUAL_FLOAT(85.0f, core::validarDS18B20(85.0f));
}

void test_sht31_temperatura(void) {
    TEST_ASSERT_EQUAL_FLOAT(5.2f, core::validarSHT31Temperatura(5.2f));
    TEST_ASSERT_TRUE(std::isnan(core::validarSHT31Temperatura(-41.0f)));
    TEST_ASSERT_TRUE(std::isnan(core::validarSHT31Temperatura(126.0f)));
    TEST_ASSERT_TRUE(std::isnan(core::validarSHT31Temperatura(NAN)));
}

void test_sht31_humedad(void) {
    TEST_ASSERT_EQUAL_FLOAT(62.0f, core::validarSHT31Humedad(62.0f));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, core::validarSHT31Humedad(0.0f));
    TEST_ASSERT_EQUAL_FLOAT(100.0f, core::validarSHT31Humedad(100.0f));
    TEST_ASSERT_TRUE(std::isnan(core::validarSHT31Humedad(-0.5f)));
    TEST_ASSERT_TRUE(std::isnan(core::validarSHT31Humedad(100.5f)));
}

/// El rango operativo de la cadena de frío (2-8 °C) NO se filtra aquí:
/// justamente las excursiones son lo que hay que detectar.
void test_las_excursiones_no_se_filtran(void) {
    TEST_ASSERT_EQUAL_FLOAT(15.0f, core::validarDS18B20(15.0f));
    TEST_ASSERT_EQUAL_FLOAT(-10.0f, core::validarDS18B20(-10.0f));
}

void run_tests_sensores(void) {
    RUN_TEST(test_ds18b20_lectura_valida_pasa);
    RUN_TEST(test_ds18b20_codigo_de_error_es_nan);
    RUN_TEST(test_ds18b20_fuera_de_rango_es_nan);
    RUN_TEST(test_ds18b20_deja_pasar_los_85_grados);
    RUN_TEST(test_sht31_temperatura);
    RUN_TEST(test_sht31_humedad);
    RUN_TEST(test_las_excursiones_no_se_filtran);
}
