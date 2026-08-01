/**
 * Buffer offline: nombres, orden FIFO e integridad (HU-06, HU-07, RNF-07).
 */
#include <unity.h>

#include <string>
#include <vector>

#include "core/ColaArchivos.h"

void test_nombre_de_archivo_tiene_cinco_digitos(void) {
    TEST_ASSERT_EQUAL_STRING("00001.json", core::nombreArchivo(1).c_str());
    TEST_ASSERT_EQUAL_STRING("00042.json", core::nombreArchivo(42).c_str());
    TEST_ASSERT_EQUAL_STRING("99999.json", core::nombreArchivo(99999).c_str());
}

void test_indice_se_lee_del_nombre(void) {
    TEST_ASSERT_EQUAL_INT(1, core::indiceDeNombre("00001.json"));
    TEST_ASSERT_EQUAL_INT(99999, core::indiceDeNombre("99999.json"));
}

/// El parseo antiguo cortaba los 5 primeros caracteres a ciegas, así que de
/// "100000.json" leía "10000": un índice ya usado. Debe rechazarse.
void test_nombres_invalidos_se_rechazan(void) {
    TEST_ASSERT_EQUAL_INT(-1, core::indiceDeNombre("100000.json"));
    TEST_ASSERT_EQUAL_INT(-1, core::indiceDeNombre("abc.json"));
    TEST_ASSERT_EQUAL_INT(-1, core::indiceDeNombre("00001.txt"));
    TEST_ASSERT_EQUAL_INT(-1, core::indiceDeNombre(".json"));
    TEST_ASSERT_EQUAL_INT(-1, core::indiceDeNombre("00001"));
}

/// Con lecturas cada 30 s el contador llega a 99999 en ~34 días de operación
/// continua. Sin vuelta explícita se emitían nombres de 6 dígitos que rompían
/// el orden FIFO justo en el escenario de uso prolongado.
void test_el_indice_da_la_vuelta_al_agotarse(void) {
    TEST_ASSERT_EQUAL_INT(2, core::siguienteIndice(1));
    TEST_ASSERT_EQUAL_INT(99999, core::siguienteIndice(99998));
    TEST_ASSERT_EQUAL_INT(1, core::siguienteIndice(99999));
    TEST_ASSERT_EQUAL_INT(1, core::siguienteIndice(0));
}

void test_orden_fifo_es_cronologico(void) {
    std::vector<std::string> v = {"00010.json", "00002.json", "00001.json", "00009.json"};
    core::ordenarFIFO(v);
    TEST_ASSERT_EQUAL_STRING("00001.json", v[0].c_str());
    TEST_ASSERT_EQUAL_STRING("00002.json", v[1].c_str());
    TEST_ASSERT_EQUAL_STRING("00009.json", v[2].c_str());
    TEST_ASSERT_EQUAL_STRING("00010.json", v[3].c_str());
}

/// Un nombre corrupto no puede bloquear el drenaje de los válidos: se ordena al
/// final para que se publique lo bueno primero y él se descarte después.
void test_nombres_corruptos_van_al_final(void) {
    std::vector<std::string> v = {"basura", "00005.json", "00001.json"};
    core::ordenarFIFO(v);
    TEST_ASSERT_EQUAL_STRING("00001.json", v[0].c_str());
    TEST_ASSERT_EQUAL_STRING("00005.json", v[1].c_str());
    TEST_ASSERT_EQUAL_STRING("basura", v[2].c_str());
}

// ---------------------------------------------------------------------------
// Integridad: corte de corriente a mitad de escritura en flash
// ---------------------------------------------------------------------------

void test_payload_completo_es_integro(void) {
    TEST_ASSERT_TRUE(core::esPayloadIntegro(
        "{\"device_id\":\"FARM-01-CDL\",\"temperatura_interna\":4.5}"));
}

void test_payload_truncado_se_rechaza(void) {
    // Exactamente lo que deja un corte de corriente a mitad de `f.print()`.
    TEST_ASSERT_FALSE(core::esPayloadIntegro("{\"device_id\":\"FARM-01-C"));
    TEST_ASSERT_FALSE(core::esPayloadIntegro("{\"device_id\":\"FARM\",\"t\":4.5"));
    TEST_ASSERT_FALSE(core::esPayloadIntegro(""));
    TEST_ASSERT_FALSE(core::esPayloadIntegro("   "));
}

void test_payload_con_llaves_desbalanceadas_se_rechaza(void) {
    TEST_ASSERT_FALSE(core::esPayloadIntegro("{\"a\":1}}"));
    TEST_ASSERT_FALSE(core::esPayloadIntegro("}{"));
}

/// Una llave dentro de una cadena no cuenta como anidamiento: si contara, un
/// `device_id` con `{` haría descartar lecturas perfectamente válidas.
void test_llaves_dentro_de_cadenas_no_cuentan(void) {
    TEST_ASSERT_TRUE(core::esPayloadIntegro("{\"device_id\":\"FARM{01}\"}"));
    TEST_ASSERT_TRUE(core::esPayloadIntegro("{\"device_id\":\"a\\\"b\"}"));
}

void test_payload_mayor_que_el_limite_se_rechaza(void) {
    const std::string grande = "{" + std::string(600, 'x') + "}";
    TEST_ASSERT_FALSE(core::esPayloadIntegro(grande));
}

void run_tests_cola(void) {
    RUN_TEST(test_nombre_de_archivo_tiene_cinco_digitos);
    RUN_TEST(test_indice_se_lee_del_nombre);
    RUN_TEST(test_nombres_invalidos_se_rechazan);
    RUN_TEST(test_el_indice_da_la_vuelta_al_agotarse);
    RUN_TEST(test_orden_fifo_es_cronologico);
    RUN_TEST(test_nombres_corruptos_van_al_final);
    RUN_TEST(test_payload_completo_es_integro);
    RUN_TEST(test_payload_truncado_se_rechaza);
    RUN_TEST(test_payload_con_llaves_desbalanceadas_se_rechaza);
    RUN_TEST(test_llaves_dentro_de_cadenas_no_cuentan);
    RUN_TEST(test_payload_mayor_que_el_limite_se_rechaza);
}
