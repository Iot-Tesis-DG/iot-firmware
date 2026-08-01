/**
 * Protocolo del buffer offline bajo intercalado adversario (HU-06, HU-07,
 * HU-11, RNF-07).
 *
 * QUÉ DEMUESTRAN Y QUÉ NO
 * -----------------------
 * La carrera real entre los dos núcleos del ESP32 no se puede reproducir en el
 * host: no hay dos schedulers. Lo que estas pruebas hacen es ejecutar, en un
 * solo hilo, los intercalados de operaciones que el planificador de FreeRTOS
 * puede producir, y comprobar que el algoritmo se mantiene correcto en todos
 * ellos.
 *
 * Es decir: demuestran la corrección del PROTOCOLO, no la del planificador ni
 * la del mutex. Que el mutex de `LittleFSBuffer` esté bien puesto sigue sin
 * tener prueba automatizada y sigue dependiendo de revisión manual.
 */
#include <unity.h>

#include <map>
#include <string>
#include <vector>

#include "core/ColaArchivos.h"
#include "core/ColaFIFO.h"

// ---------------------------------------------------------------------------
// Almacén falso en memoria, con inyección de fallos de flash
// ---------------------------------------------------------------------------
class AlmacenMemoria : public core::AlmacenLecturas {
public:
    std::map<std::string, std::string> archivos;

    /// Si > 0, la siguiente escritura solo graba estos bytes (corte de
    /// corriente a mitad de `f.print()`).
    size_t truncarSiguienteEscrituraA = 0;
    /// Si true, la siguiente escritura falla del todo (flash llena).
    bool fallarSiguienteEscritura = false;

    std::vector<std::string> listar() override {
        std::vector<std::string> r;
        for (const auto& kv : archivos) r.push_back(kv.first);
        return r;
    }
    bool existe(const std::string& n) override { return archivos.count(n) > 0; }

    size_t escribir(const std::string& n, const std::string& c) override {
        if (fallarSiguienteEscritura) {
            fallarSiguienteEscritura = false;
            return 0;
        }
        if (truncarSiguienteEscrituraA > 0) {
            const size_t corte = truncarSiguienteEscrituraA;
            truncarSiguienteEscrituraA = 0;
            archivos[n] = c.substr(0, corte);
            return corte;
        }
        archivos[n] = c;
        return c.size();
    }
    bool leer(const std::string& n, std::string& salida) override {
        auto it = archivos.find(n);
        if (it == archivos.end()) return false;
        salida = it->second;
        return true;
    }
    bool borrar(const std::string& n) override { return archivos.erase(n) > 0; }
};

/// Publicador falso: guarda lo publicado y permite programar el resultado.
class PublicadorFalso : public core::Publicador {
public:
    std::vector<std::string> publicados;
    std::vector<core::ResultadoPublicacion> guion;  // resultado por llamada
    size_t llamada = 0;

    core::ResultadoPublicacion publicar(const std::string& payload) override {
        core::ResultadoPublicacion r = core::ResultadoPublicacion::Confirmado;
        if (llamada < guion.size()) r = guion[llamada];
        llamada++;
        if (r == core::ResultadoPublicacion::Confirmado) publicados.push_back(payload);
        return r;
    }
};

static std::string lectura(const std::string& id) {
    return "{\"device_id\":\"FARM-01-CDL\",\"v\":\"" + id + "\"}";
}

// ---------------------------------------------------------------------------
// Índices y ABA
// ---------------------------------------------------------------------------

/// El riesgo ABA: `drenarBuffer()` toma una instantánea de nombres y la
/// procesa. Si el contador se recalculara listando el directorio (último+1) y
/// la cola se vaciara entre medias, el contador reiniciaría a 1 y un nombre de
/// esa instantánea pasaría a referirse a una lectura DISTINTA de la que se
/// publicó: el borrado se llevaría por delante un dato nunca enviado.
void test_indice_no_se_reutiliza_tras_vaciar_la_cola(void) {
    AlmacenMemoria alm;
    core::ColaFIFO cola(alm, 200, 512);

    TEST_ASSERT_TRUE(cola.guardar(lectura("A")));
    TEST_ASSERT_EQUAL_STRING("00001.json", cola.pendientes()[0].c_str());

    // El Core 1 drena la cola por completo.
    cola.eliminar("00001.json");
    TEST_ASSERT_EQUAL_UINT(0, (unsigned)cola.cuenta());

    // El Core 0 guarda otra lectura: NO puede volver a llamarse 00001.
    TEST_ASSERT_TRUE(cola.guardar(lectura("B")));
    TEST_ASSERT_EQUAL_STRING("00002.json", cola.pendientes()[0].c_str());
}

/// Un reinicio del nodo no puede reutilizar nombres de lecturas que siguen
/// pendientes de publicar.
void test_reinicio_siembra_el_indice_desde_disco(void) {
    AlmacenMemoria alm;
    {
        core::ColaFIFO cola(alm, 200, 512);
        cola.guardar(lectura("A"));
        cola.guardar(lectura("B"));
        cola.guardar(lectura("C"));
    }
    // Corte de corriente: se pierde el contador en RAM, no los archivos.
    core::ColaFIFO trasReinicio(alm, 200, 512);
    trasReinicio.sembrarIndice();
    TEST_ASSERT_EQUAL_INT(3, trasReinicio.ultimoIndice());

    TEST_ASSERT_TRUE(trasReinicio.guardar(lectura("D")));
    const auto p = trasReinicio.pendientes();
    TEST_ASSERT_EQUAL_UINT(4, (unsigned)p.size());
    TEST_ASSERT_EQUAL_STRING("00004.json", p[3].c_str());
}

// ---------------------------------------------------------------------------
// Intercalado Core 0 / Core 1
// ---------------------------------------------------------------------------

/// El Core 0 escribe mientras el Core 1 está a mitad del drenaje. Ninguna
/// lectura puede perderse ni adelantarse a otra más antigua.
void test_intercalado_core0_escribe_mientras_core1_drena(void) {
    AlmacenMemoria alm;
    core::ColaFIFO cola(alm, 200, 512);
    PublicadorFalso pub;

    cola.guardar(lectura("A"));
    cola.guardar(lectura("B"));

    // Core 1: instantánea de la cola.
    const auto instantanea = cola.pendientes();
    TEST_ASSERT_EQUAL_UINT(2, (unsigned)instantanea.size());

    // Core 1: publica y borra el primero.
    std::string payload;
    TEST_ASSERT_TRUE(cola.leerIntegro(instantanea[0], payload));
    TEST_ASSERT_EQUAL(core::ResultadoPublicacion::Confirmado, pub.publicar(payload));
    cola.eliminar(instantanea[0]);

    // Core 0 SE INTERCALA aquí: guarda una lectura nueva.
    TEST_ASSERT_TRUE(cola.guardar(lectura("C")));

    // Core 1: continúa con su instantánea, que sigue siendo válida.
    TEST_ASSERT_TRUE(cola.leerIntegro(instantanea[1], payload));
    TEST_ASSERT_EQUAL(core::ResultadoPublicacion::Confirmado, pub.publicar(payload));
    cola.eliminar(instantanea[1]);

    // Queda exactamente la lectura que el Core 0 escribió durante el drenaje.
    const auto restantes = cola.pendientes();
    TEST_ASSERT_EQUAL_UINT(1, (unsigned)restantes.size());
    TEST_ASSERT_EQUAL_STRING("00003.json", restantes[0].c_str());

    TEST_ASSERT_EQUAL_UINT(2, (unsigned)pub.publicados.size());
    TEST_ASSERT_EQUAL_STRING(lectura("A").c_str(), pub.publicados[0].c_str());
    TEST_ASSERT_EQUAL_STRING(lectura("B").c_str(), pub.publicados[1].c_str());
}

/// El Core 0 satura la cola mientras el Core 1 tiene una instantánea antigua:
/// el Core 1 debe tolerar que un nombre de su lista ya no exista.
void test_core1_tolera_que_el_fifo_borre_un_archivo_de_su_instantanea(void) {
    AlmacenMemoria alm;
    core::ColaFIFO cola(alm, 3, 512);  // cola diminuta para forzar el FIFO
    PublicadorFalso pub;

    cola.guardar(lectura("A"));
    cola.guardar(lectura("B"));
    const auto instantanea = cola.pendientes();

    // Core 0 satura: el FIFO descarta el más antiguo (00001 = "A").
    cola.guardar(lectura("C"));
    cola.guardar(lectura("D"));

    // Core 1 intenta leer 00001, que ya no existe. No debe romperse.
    std::string payload;
    TEST_ASSERT_FALSE(cola.leerIntegro(instantanea[0], payload));

    // Y `eliminar` sobre un archivo inexistente devuelve false sin efectos.
    TEST_ASSERT_FALSE(cola.eliminar(instantanea[0]));
}

// ---------------------------------------------------------------------------
// Garantía de entrega: borrar SOLO tras PUBACK confirmado
// ---------------------------------------------------------------------------

/// El defecto que motivó la migración de PubSubClient: con QoS 0 el archivo se
/// borraba tras "escrito en el socket". Ahora solo se borra con `Confirmado`.
void test_no_se_borra_nada_sin_puback_confirmado(void) {
    AlmacenMemoria alm;
    core::ColaFIFO cola(alm, 200, 512);
    PublicadorFalso pub;

    cola.guardar(lectura("A"));
    cola.guardar(lectura("B"));

    pub.guion = {core::ResultadoPublicacion::Fallo};

    const core::ResumenDrenaje r = core::drenar(cola, pub, 20);
    TEST_ASSERT_EQUAL_INT(0, r.confirmados);
    TEST_ASSERT_TRUE(r.detenido);

    // Las DOS lecturas siguen en la cola: nada se publicó, nada se borró.
    TEST_ASSERT_EQUAL_UINT(2, (unsigned)cola.cuenta());
    TEST_ASSERT_EQUAL_UINT(0, (unsigned)pub.publicados.size());
}

/// Se detiene al primer fallo: el orden cronológico de la cadena de evidencia
/// importa más que el rendimiento.
void test_el_drenaje_se_detiene_al_primer_fallo_y_conserva_el_resto(void) {
    AlmacenMemoria alm;
    core::ColaFIFO cola(alm, 200, 512);
    PublicadorFalso pub;

    for (int i = 0; i < 5; i++) cola.guardar(lectura(std::to_string(i)));

    pub.guion = {core::ResultadoPublicacion::Confirmado,
                 core::ResultadoPublicacion::Confirmado,
                 core::ResultadoPublicacion::SinConexion};

    const core::ResumenDrenaje r = core::drenar(cola, pub, 20);
    TEST_ASSERT_EQUAL_INT(2, r.confirmados);
    TEST_ASSERT_TRUE(r.detenido);
    TEST_ASSERT_EQUAL_UINT(3, (unsigned)cola.cuenta());

    // Y lo que queda son exactamente las tres más recientes, en orden.
    const auto restantes = cola.pendientes();
    TEST_ASSERT_EQUAL_STRING("00003.json", restantes[0].c_str());
    TEST_ASSERT_EQUAL_STRING("00005.json", restantes[2].c_str());
}

/// Un reintento tras un fallo reenvía la MISMA lectura. Es correcto: el backend
/// deduplica por UNIQUE(device_id, timestamp), así que QoS 1 puede duplicar sin
/// consecuencias, pero jamás debe perder.
void test_tras_un_fallo_se_reintenta_la_misma_lectura(void) {
    AlmacenMemoria alm;
    core::ColaFIFO cola(alm, 200, 512);

    cola.guardar(lectura("A"));

    PublicadorFalso fallo;
    fallo.guion = {core::ResultadoPublicacion::Fallo};
    core::drenar(cola, fallo, 20);
    TEST_ASSERT_EQUAL_UINT(1, (unsigned)cola.cuenta());

    PublicadorFalso exito;
    const core::ResumenDrenaje r = core::drenar(cola, exito, 20);
    TEST_ASSERT_EQUAL_INT(1, r.confirmados);
    TEST_ASSERT_EQUAL_UINT(0, (unsigned)cola.cuenta());
    TEST_ASSERT_EQUAL_STRING(lectura("A").c_str(), exito.publicados[0].c_str());
}

/// RNF-07: la cola se drena en lotes acotados para no agotar el plazo del
/// watchdog encadenando PUBACKs.
void test_el_drenaje_respeta_el_tope_por_ciclo(void) {
    AlmacenMemoria alm;
    core::ColaFIFO cola(alm, 200, 512);
    PublicadorFalso pub;

    for (int i = 0; i < 50; i++) cola.guardar(lectura(std::to_string(i)));

    const core::ResumenDrenaje r = core::drenar(cola, pub, 20);
    TEST_ASSERT_EQUAL_INT(20, r.confirmados);
    TEST_ASSERT_EQUAL_UINT(30, (unsigned)cola.cuenta());
}

// ---------------------------------------------------------------------------
// Integridad frente a cortes de corriente
// ---------------------------------------------------------------------------

/// Un archivo truncado no puede bloquear la cola: se descarta y el drenaje
/// continúa con las lecturas posteriores, que sí son válidas.
void test_archivo_truncado_se_descarta_sin_bloquear_la_cola(void) {
    AlmacenMemoria alm;
    core::ColaFIFO cola(alm, 200, 512);
    PublicadorFalso pub;

    cola.guardar(lectura("A"));
    cola.guardar(lectura("B"));
    cola.guardar(lectura("C"));

    // Corte de corriente a mitad de escritura sobre el segundo archivo.
    alm.archivos["00002.json"] = "{\"device_id\":\"FARM-01-C";

    const core::ResumenDrenaje r = core::drenar(cola, pub, 20);
    TEST_ASSERT_EQUAL_INT(2, r.confirmados);
    TEST_ASSERT_EQUAL_INT(1, r.descartados);
    TEST_ASSERT_FALSE(r.detenido);
    TEST_ASSERT_EQUAL_UINT(0, (unsigned)cola.cuenta());

    TEST_ASSERT_EQUAL_UINT(2, (unsigned)pub.publicados.size());
    TEST_ASSERT_EQUAL_STRING(lectura("A").c_str(), pub.publicados[0].c_str());
    TEST_ASSERT_EQUAL_STRING(lectura("C").c_str(), pub.publicados[1].c_str());
}

/// Una escritura parcial no puede dejar un archivo truncado en la cola: se
/// borra en el acto y `guardar()` informa del fallo.
void test_escritura_parcial_no_deja_archivo_en_la_cola(void) {
    AlmacenMemoria alm;
    core::ColaFIFO cola(alm, 200, 512);

    alm.truncarSiguienteEscrituraA = 10;
    TEST_ASSERT_FALSE(cola.guardar(lectura("A")));
    TEST_ASSERT_EQUAL_UINT(0, (unsigned)cola.cuenta());
}

void test_flash_llena_no_deja_archivo_en_la_cola(void) {
    AlmacenMemoria alm;
    core::ColaFIFO cola(alm, 200, 512);

    alm.fallarSiguienteEscritura = true;
    TEST_ASSERT_FALSE(cola.guardar(lectura("A")));
    TEST_ASSERT_EQUAL_UINT(0, (unsigned)cola.cuenta());
}

// ---------------------------------------------------------------------------
// Saturación
// ---------------------------------------------------------------------------

void test_fifo_descarta_la_lectura_mas_antigua(void) {
    AlmacenMemoria alm;
    core::ColaFIFO cola(alm, 3, 512);

    cola.guardar(lectura("A"));
    cola.guardar(lectura("B"));
    cola.guardar(lectura("C"));
    TEST_ASSERT_EQUAL_UINT(3, (unsigned)cola.cuenta());

    cola.guardar(lectura("D"));
    TEST_ASSERT_EQUAL_UINT(3, (unsigned)cola.cuenta());

    // Se sacrificó la más antigua, no la más reciente.
    const auto p = cola.pendientes();
    TEST_ASSERT_EQUAL_STRING("00002.json", p[0].c_str());
    TEST_ASSERT_EQUAL_STRING("00004.json", p[2].c_str());

    std::string payload;
    cola.leerIntegro(p[0], payload);
    TEST_ASSERT_EQUAL_STRING(lectura("B").c_str(), payload.c_str());
}

void test_payload_fuera_de_limite_no_se_guarda(void) {
    AlmacenMemoria alm;
    core::ColaFIFO cola(alm, 200, 512);

    TEST_ASSERT_FALSE(cola.guardar(""));
    TEST_ASSERT_FALSE(cola.guardar(std::string(600, 'x')));
    TEST_ASSERT_EQUAL_UINT(0, (unsigned)cola.cuenta());
}

/// La vuelta del índice a los 99999 archivos debe funcionar sobre la cola real,
/// no solo en la aritmética: el nombre reaparece y hay que liberar el anterior.
void test_la_vuelta_del_indice_libera_el_archivo_previo(void) {
    AlmacenMemoria alm;
    core::ColaFIFO cola(alm, 200, 512);

    // Se coloca el contador justo antes del límite del formato de 5 dígitos.
    alm.archivos["99999.json"] = lectura("viejo");
    cola.sembrarIndice();
    TEST_ASSERT_EQUAL_INT(99999, cola.ultimoIndice());

    // La siguiente da la vuelta a 00001.
    TEST_ASSERT_TRUE(cola.guardar(lectura("nuevo")));
    TEST_ASSERT_EQUAL_INT(1, cola.ultimoIndice());

    std::string payload;
    TEST_ASSERT_TRUE(cola.leerIntegro("00001.json", payload));
    TEST_ASSERT_EQUAL_STRING(lectura("nuevo").c_str(), payload.c_str());
}

void test_drenar_una_cola_vacia_no_hace_nada(void) {
    AlmacenMemoria alm;
    core::ColaFIFO cola(alm, 200, 512);
    PublicadorFalso pub;

    const core::ResumenDrenaje r = core::drenar(cola, pub, 20);
    TEST_ASSERT_EQUAL_INT(0, r.confirmados);
    TEST_ASSERT_EQUAL_INT(0, r.descartados);
    TEST_ASSERT_FALSE(r.detenido);
    TEST_ASSERT_EQUAL_UINT(0, (unsigned)pub.llamada);
}

/// Una cola entera de archivos corruptos debe vaciarse, no bloquearse: si no,
/// el nodo se quedaría atascado publicando nada para siempre.
void test_una_cola_entera_de_basura_se_purga(void) {
    AlmacenMemoria alm;
    core::ColaFIFO cola(alm, 200, 512);
    PublicadorFalso pub;

    for (int i = 1; i <= 5; i++) {
        alm.archivos[core::nombreArchivo(i)] = "{\"truncado";
    }

    const core::ResumenDrenaje r = core::drenar(cola, pub, 20);
    TEST_ASSERT_EQUAL_INT(0, r.confirmados);
    TEST_ASSERT_EQUAL_INT(5, r.descartados);
    TEST_ASSERT_FALSE(r.detenido);
    TEST_ASSERT_EQUAL_UINT(0, (unsigned)cola.cuenta());
}

void run_tests_cola_fifo(void) {
    RUN_TEST(test_la_vuelta_del_indice_libera_el_archivo_previo);
    RUN_TEST(test_drenar_una_cola_vacia_no_hace_nada);
    RUN_TEST(test_una_cola_entera_de_basura_se_purga);
    RUN_TEST(test_indice_no_se_reutiliza_tras_vaciar_la_cola);
    RUN_TEST(test_reinicio_siembra_el_indice_desde_disco);
    RUN_TEST(test_intercalado_core0_escribe_mientras_core1_drena);
    RUN_TEST(test_core1_tolera_que_el_fifo_borre_un_archivo_de_su_instantanea);
    RUN_TEST(test_no_se_borra_nada_sin_puback_confirmado);
    RUN_TEST(test_el_drenaje_se_detiene_al_primer_fallo_y_conserva_el_resto);
    RUN_TEST(test_tras_un_fallo_se_reintenta_la_misma_lectura);
    RUN_TEST(test_el_drenaje_respeta_el_tope_por_ciclo);
    RUN_TEST(test_archivo_truncado_se_descarta_sin_bloquear_la_cola);
    RUN_TEST(test_escritura_parcial_no_deja_archivo_en_la_cola);
    RUN_TEST(test_flash_llena_no_deja_archivo_en_la_cola);
    RUN_TEST(test_fifo_descarta_la_lectura_mas_antigua);
    RUN_TEST(test_payload_fuera_de_limite_no_se_guarda);
}
