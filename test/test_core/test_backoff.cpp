/**
 * Backoff exponencial de reconexión Wi-Fi (HU-08).
 */
#include <unity.h>

#include "core/Backoff.h"

void test_secuencia_documentada_1s_a_60s(void) {
    core::Backoff b(1000, 60000, 2);
    TEST_ASSERT_EQUAL_UINT32(1000, b.retardoMs());

    const uint32_t esperados[] = {2000, 4000, 8000, 16000, 32000, 60000, 60000};
    for (uint32_t esperado : esperados) {
        b.registrarFallo();
        TEST_ASSERT_EQUAL_UINT32(esperado, b.retardoMs());
    }
}

void test_el_tope_no_se_supera_nunca(void) {
    core::Backoff b(1000, 60000, 2);
    for (int i = 0; i < 100; i++) b.registrarFallo();
    TEST_ASSERT_EQUAL_UINT32(60000, b.retardoMs());
}

void test_una_conexion_correcta_reinicia_el_backoff(void) {
    core::Backoff b(1000, 60000, 2);
    b.registrarFallo();
    b.registrarFallo();
    b.registrarFallo();
    TEST_ASSERT_EQUAL_UINT32(3, b.intentos());

    b.registrarExito();
    TEST_ASSERT_EQUAL_UINT32(1000, b.retardoMs());
    TEST_ASSERT_EQUAL_UINT32(0, b.intentos());
}

void test_no_se_reintenta_antes_de_que_venza_el_retardo(void) {
    core::Backoff b(1000, 60000, 2);
    TEST_ASSERT_FALSE(b.debeIntentar(500, 0));
    TEST_ASSERT_TRUE(b.debeIntentar(1000, 0));
    TEST_ASSERT_TRUE(b.debeIntentar(5000, 0));

    b.registrarFallo();  // ahora 2000 ms
    TEST_ASSERT_FALSE(b.debeIntentar(1999, 0));
    TEST_ASSERT_TRUE(b.debeIntentar(2000, 0));
}

/// Si la ventana de reintento cae justo sobre el desbordamiento de `millis()`,
/// una resta con signo daría un valor negativo enorme y el nodo dejaría de
/// reintentar la conexión hasta el siguiente reinicio.
void test_el_desbordamiento_de_millis_no_bloquea_los_reintentos(void) {
    core::Backoff b(1000, 60000, 2);
    const uint32_t ultimo = 0xFFFFFC18u;   // 1000 ms antes de dar la vuelta
    TEST_ASSERT_FALSE(b.debeIntentar(0xFFFFFE00u, ultimo));
    TEST_ASSERT_TRUE(b.debeIntentar(0x00000000u, ultimo));  // ya desbordado
    TEST_ASSERT_TRUE(b.debeIntentar(0x00000064u, ultimo));
}

/// Un factor de 1 dejaría el backoff plano: reintentos cada segundo para
/// siempre, que es justo lo que la política pretende evitar.
void test_factor_degenerado_se_corrige(void) {
    core::Backoff b(1000, 60000, 1);
    b.registrarFallo();
    TEST_ASSERT_TRUE(b.retardoMs() > 1000);
}

void run_tests_backoff(void) {
    RUN_TEST(test_secuencia_documentada_1s_a_60s);
    RUN_TEST(test_el_tope_no_se_supera_nunca);
    RUN_TEST(test_una_conexion_correcta_reinicia_el_backoff);
    RUN_TEST(test_no_se_reintenta_antes_de_que_venza_el_retardo);
    RUN_TEST(test_el_desbordamiento_de_millis_no_bloquea_los_reintentos);
    RUN_TEST(test_factor_degenerado_se_corrige);
}
