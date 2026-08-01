/**
 * Punto de entrada de las pruebas unitarias del host (`pio test -e native`).
 *
 * Un único binario Unity para todos los módulos de `src/core/`: son pruebas de
 * lógica pura, se ejecutan en milisegundos y compartir el runner evita seis
 * enlaces separados en el CI.
 */
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

void run_tests_payload(void);
void run_tests_cola(void);
void run_tests_cola_fifo(void);
void run_tests_antirrebote(void);
void run_tests_backoff(void);
void run_tests_sensores(void);
void run_tests_reloj(void);
void run_tests_credenciales(void);

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    UNITY_BEGIN();
    run_tests_payload();
    run_tests_cola();
    run_tests_cola_fifo();
    run_tests_antirrebote();
    run_tests_backoff();
    run_tests_sensores();
    run_tests_reloj();
    run_tests_credenciales();
    return UNITY_END();
}
