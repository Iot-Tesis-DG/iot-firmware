/**
 * Reglas de aprovisionamiento (RNF-05: cero credenciales embebidas).
 */
#include <unity.h>

#include "core/Credenciales.h"

/// Estos valores estaban compilados dentro del binario. Deben tratarse como
/// "no aprovisionado", no como credenciales.
void test_los_marcadores_del_repositorio_no_valen_como_credencial(void) {
    TEST_ASSERT_FALSE(core::credencialValida("cambiar_en_produccion"));
    TEST_ASSERT_FALSE(core::credencialValida("token_generado_en_emqx"));
    TEST_ASSERT_FALSE(core::credencialValida("tu-instancia.emqx.cloud"));
    TEST_ASSERT_FALSE(core::credencialValida("placeholder-ci"));
}

void test_vacio_y_espacios_no_valen(void) {
    TEST_ASSERT_FALSE(core::credencialValida(""));
    TEST_ASSERT_FALSE(core::credencialValida("   "));
    TEST_ASSERT_FALSE(core::credencialValida("\t\n"));
}

void test_una_credencial_real_vale(void) {
    TEST_ASSERT_TRUE(core::credencialValida("a1b2c3d4e5f60718"));
    TEST_ASSERT_TRUE(core::credencialValida("abc123.emqx.cloud"));
    TEST_ASSERT_TRUE(core::credencialValida("THERMOSAFE_IOT"));
}

void test_nodo_sin_aprovisionar_se_detecta(void) {
    TEST_ASSERT_FALSE(core::aprovisionamientoCompleto("", "", ""));
    TEST_ASSERT_FALSE(core::aprovisionamientoCompleto(
        "THERMOSAFE_IOT", "tu-instancia.emqx.cloud", "token_generado_en_emqx"));
    TEST_ASSERT_FALSE(core::aprovisionamientoCompleto(
        "THERMOSAFE_IOT", "abc123.emqx.cloud", ""));
}

/// Una red Wi-Fi abierta (sin contraseña) es una configuración legítima de
/// laboratorio: no debe bloquear el aprovisionamiento.
void test_nodo_aprovisionado_se_acepta(void) {
    TEST_ASSERT_TRUE(core::aprovisionamientoCompleto(
        "THERMOSAFE_IOT", "abc123.emqx.cloud", "a1b2c3d4e5f60718"));
}

void run_tests_credenciales(void) {
    RUN_TEST(test_los_marcadores_del_repositorio_no_valen_como_credencial);
    RUN_TEST(test_vacio_y_espacios_no_valen);
    RUN_TEST(test_una_credencial_real_vale);
    RUN_TEST(test_nodo_sin_aprovisionar_se_detecta);
    RUN_TEST(test_nodo_aprovisionado_se_acepta);
}
