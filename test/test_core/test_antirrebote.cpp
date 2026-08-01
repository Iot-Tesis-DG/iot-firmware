/**
 * Antirrebote del reed switch MC-38 y ventana de reporte (HU-04 / RF-03).
 *
 * El tiempo se inyecta, así que se puede reproducir un tren de rebotes
 * mecánicos y una apertura de 15 s entre dos payloads sin tocar hardware.
 */
#include <unity.h>

#include "core/Antirrebote.h"

void test_rebotes_no_cuentan_como_aperturas(void) {
    core::Antirrebote ar(50);
    ar.inicializar(false, 0);  // puerta cerrada

    // Tren de rebotes de 5 ms: exactamente lo que produce el contacto del reed
    // switch al cerrarse. Sin antirrebote, cada flanco sería una apertura.
    uint32_t t = 0;
    for (int i = 0; i < 10; i++) {
        t += 5;
        ar.muestrear(i % 2 == 0, t);
    }
    TEST_ASSERT_FALSE(ar.estaAbierta());
    TEST_ASSERT_FALSE(ar.huboApertura());
}

void test_apertura_estable_se_detecta(void) {
    core::Antirrebote ar(50);
    ar.inicializar(false, 0);

    ar.muestrear(true, 100);   // primer flanco: arranca el temporizador
    TEST_ASSERT_FALSE(ar.estaAbierta());
    ar.muestrear(true, 130);   // 30 ms: todavía no
    TEST_ASSERT_FALSE(ar.estaAbierta());
    ar.muestrear(true, 155);   // 55 ms estable: se acepta
    TEST_ASSERT_TRUE(ar.estaAbierta());
    TEST_ASSERT_TRUE(ar.huboApertura());
}

/// El defecto que motivó la separación entre muestreo y reporte: una apertura
/// de 15 s que empieza y termina entre dos payloads era completamente
/// invisible si solo se miraba el estado instantáneo cada 30 s.
void test_apertura_entre_dos_reportes_no_se_pierde(void) {
    core::Antirrebote ar(50);
    ar.inicializar(false, 0);

    ar.muestrear(true, 5000);
    ar.muestrear(true, 5100);           // abierta en t=5100
    ar.muestrear(false, 20000);
    ar.muestrear(false, 20100);         // cerrada en t=20100

    TEST_ASSERT_FALSE(ar.estaAbierta());              // cerrada al reportar...
    TEST_ASSERT_TRUE(ar.huboApertura());              // ...pero SÍ hubo apertura
    TEST_ASSERT_EQUAL_UINT32(15, ar.duracionSegundos(30000));
}

void test_cerrar_ventana_reinicia_el_conteo(void) {
    core::Antirrebote ar(50);
    ar.inicializar(false, 0);
    ar.muestrear(true, 1000);
    ar.muestrear(true, 1100);
    ar.muestrear(false, 6000);
    ar.muestrear(false, 6100);

    TEST_ASSERT_TRUE(ar.huboApertura());
    ar.cerrarVentana(30000);
    TEST_ASSERT_FALSE(ar.huboApertura());
    TEST_ASSERT_EQUAL_UINT32(0, ar.duracionSegundos(30000));
}

/// Si la puerta sigue abierta al cerrar la ventana, la siguiente ventana debe
/// seguir reportándola abierta: es una excursión en curso, no un evento pasado.
void test_puerta_abierta_sigue_reportandose_en_la_ventana_siguiente(void) {
    core::Antirrebote ar(50);
    ar.inicializar(false, 0);
    ar.muestrear(true, 1000);
    ar.muestrear(true, 1100);
    TEST_ASSERT_TRUE(ar.estaAbierta());

    ar.cerrarVentana(30000);
    TEST_ASSERT_TRUE(ar.huboApertura());
    TEST_ASSERT_EQUAL_UINT32(0, ar.duracionSegundos(30000));
    TEST_ASSERT_EQUAL_UINT32(10, ar.duracionSegundos(40000));
}

void test_aperturas_multiples_se_acumulan(void) {
    core::Antirrebote ar(50);
    ar.inicializar(false, 0);

    ar.muestrear(true, 1000);  ar.muestrear(true, 1100);    // abre  t=1100
    ar.muestrear(false, 4000); ar.muestrear(false, 4100);   // cierra t=4100  (3 s)
    ar.muestrear(true, 6000);  ar.muestrear(true, 6100);    // abre  t=6100
    ar.muestrear(false, 9000); ar.muestrear(false, 9100);   // cierra t=9100  (3 s)

    TEST_ASSERT_EQUAL_UINT32(6, ar.duracionSegundos(20000));
}

/// `millis()` desborda a los ~49.7 días. Con resta sin signo la diferencia
/// sigue siendo correcta; con una comparación ingenua, el antirrebote se
/// quedaría bloqueado justo en ese punto.
void test_el_desbordamiento_de_millis_no_rompe_el_antirrebote(void) {
    core::Antirrebote ar(50);
    const uint32_t casiMax = 0xFFFFFF00u;  // ~256 ms antes de dar la vuelta
    ar.inicializar(false, casiMax);

    ar.muestrear(true, casiMax + 10);
    ar.muestrear(true, casiMax + 100);   // desborda a mitad: 100 ms estable
    TEST_ASSERT_TRUE(ar.estaAbierta());
    TEST_ASSERT_TRUE(ar.huboApertura());
}

void test_arranque_con_la_puerta_abierta(void) {
    core::Antirrebote ar(50);
    ar.inicializar(true, 0);
    TEST_ASSERT_TRUE(ar.estaAbierta());
    TEST_ASSERT_TRUE(ar.huboApertura());
    TEST_ASSERT_EQUAL_UINT32(5, ar.duracionSegundos(5000));
}

void run_tests_antirrebote(void) {
    RUN_TEST(test_rebotes_no_cuentan_como_aperturas);
    RUN_TEST(test_apertura_estable_se_detecta);
    RUN_TEST(test_apertura_entre_dos_reportes_no_se_pierde);
    RUN_TEST(test_cerrar_ventana_reinicia_el_conteo);
    RUN_TEST(test_puerta_abierta_sigue_reportandose_en_la_ventana_siguiente);
    RUN_TEST(test_aperturas_multiples_se_acumulan);
    RUN_TEST(test_el_desbordamiento_de_millis_no_rompe_el_antirrebote);
    RUN_TEST(test_arranque_con_la_puerta_abierta);
}
