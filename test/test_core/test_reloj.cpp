/**
 * Reloj UTC y formato ISO 8601 del timestamp.
 *
 * El backend valida el timestamp con una ventana de ±2 h y rechaza lo que
 * quede fuera. Un error aquí no produce un dato erróneo: produce silencio, que
 * es peor, porque el nodo parece estar funcionando.
 */
#include <unity.h>

#include "core/Reloj.h"

void test_formato_iso8601(void) {
    // 2026-07-25T12:34:56Z
    TEST_ASSERT_EQUAL_STRING("2026-07-25T12:34:56Z",
                             core::formatearISO8601(1784982896).c_str());
    TEST_ASSERT_EQUAL_STRING("1970-01-01T00:00:00Z", core::formatearISO8601(0).c_str());
}

void test_anio_bisiesto(void) {
    // 2024-02-29T00:00:00Z
    TEST_ASSERT_EQUAL_STRING("2024-02-29T00:00:00Z",
                             core::formatearISO8601(1709164800).c_str());
}

void test_hora_de_compilacion_se_interpreta_como_utc(void) {
    const time_t e = core::epocaDeCompilacion("Jul 25 2026", "12:34:56");
    TEST_ASSERT_EQUAL_STRING("2026-07-25T12:34:56Z", core::formatearISO8601(e).c_str());
}

/// `__DATE__` rellena el día con un espacio cuando es de una cifra
/// ("Jul  5 2026"): `sscanf` con "%s %d %d" lo absorbe correctamente.
void test_dia_de_una_cifra(void) {
    const time_t e = core::epocaDeCompilacion("Jul  5 2026", "01:02:03");
    TEST_ASSERT_EQUAL_STRING("2026-07-05T01:02:03Z", core::formatearISO8601(e).c_str());
}

void test_fecha_de_compilacion_invalida_devuelve_cero(void) {
    TEST_ASSERT_EQUAL_INT(0, (int)core::epocaDeCompilacion("Xyz 25 2026", "12:34:56"));
    TEST_ASSERT_EQUAL_INT(0, (int)core::epocaDeCompilacion("basura", "12:34:56"));
    TEST_ASSERT_EQUAL_INT(0, (int)core::epocaDeCompilacion("Jul 25 2026", "basura"));
    TEST_ASSERT_EQUAL_INT(0, (int)core::epocaDeCompilacion(nullptr, nullptr));
}

void test_el_reloj_avanza_con_millis(void) {
    core::Reloj r;
    r.fijarBase(1784982896, 1000);
    TEST_ASSERT_TRUE(r.sincronizado());
    TEST_ASSERT_EQUAL_INT(1784982896, (int)r.avanzar(1000));
    TEST_ASSERT_EQUAL_INT(1784982926, (int)r.avanzar(31000));  // +30 s
}

/// Los milisegundos sobrantes de cada avance se conservan: sin eso, con un
/// ciclo de 30.5 s se perdería medio segundo por lectura y a las 24 h el
/// timestamp acumularía casi un minuto de desfase.
void test_no_se_pierden_los_milisegundos_sobrantes(void) {
    core::Reloj r;
    r.fijarBase(1000, 0);
    for (int i = 1; i <= 4; i++) r.avanzar((uint32_t)(i * 500));
    TEST_ASSERT_EQUAL_INT(1002, (int)r.avanzar(2000));
}

/// El defecto real: `epochBase + (millis() - millisBase)/1000` es correcto
/// mientras la distancia quepa en 32 bits. Un nodo con más de 49 días de
/// uptime sin resincronizar NTP veía la diferencia dar la vuelta y el
/// timestamp retrocedía casi dos meses, con el backend rechazándolo en
/// silencio. Al rebasar la referencia en cada llamada, el desbordamiento entre
/// dos llamadas consecutivas (30 s) es inofensivo.
void test_el_desbordamiento_de_millis_no_retrasa_el_reloj(void) {
    core::Reloj r;
    const uint32_t casiMax = 0xFFFFFFFFu - 10000u;  // 10 s antes de dar la vuelta
    r.fijarBase(1784982896, casiMax);

    const time_t t1 = r.avanzar(casiMax + 5000u);          // +5 s
    const time_t t2 = r.avanzar((uint32_t)(casiMax + 35000u));  // +30 s, ya desbordado

    TEST_ASSERT_EQUAL_INT(1784982901, (int)t1);
    TEST_ASSERT_EQUAL_INT(1784982931, (int)t2);
    TEST_ASSERT_TRUE(t2 > t1);
}

void test_base_no_sincronizada_se_marca_como_tal(void) {
    core::Reloj r;
    TEST_ASSERT_FALSE(r.sincronizado());
    TEST_ASSERT_FALSE(r.tieneBase());

    r.fijarBaseNoSincronizada(1784982896, 0);
    TEST_ASSERT_FALSE(r.sincronizado());
    TEST_ASSERT_TRUE(r.tieneBase());

    r.fijarBase(1784982896, 0);
    TEST_ASSERT_TRUE(r.sincronizado());
}

void run_tests_reloj(void) {
    RUN_TEST(test_formato_iso8601);
    RUN_TEST(test_anio_bisiesto);
    RUN_TEST(test_hora_de_compilacion_se_interpreta_como_utc);
    RUN_TEST(test_dia_de_una_cifra);
    RUN_TEST(test_fecha_de_compilacion_invalida_devuelve_cero);
    RUN_TEST(test_el_reloj_avanza_con_millis);
    RUN_TEST(test_no_se_pierden_los_milisegundos_sobrantes);
    RUN_TEST(test_el_desbordamiento_de_millis_no_retrasa_el_reloj);
    RUN_TEST(test_base_no_sincronizada_se_marca_como_tal);
}
