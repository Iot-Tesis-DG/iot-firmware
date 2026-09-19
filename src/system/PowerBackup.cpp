#include "PowerBackup.h"

#include <Arduino.h>

#include "../config.h"

#if RESPALDO_INSTALADO

static bool _conSuministro = true;
// Evita reavisar "nivel bajo" en cada chequeo mientras el respaldo sigue
// activo: se reabre solo al iniciar un nuevo episodio de corte.
static bool _avisoNivelBajoEnviado = false;

void inicializarRespaldo() {
    pinMode(PIN_RESPALDO_SENSE, INPUT);
    // HIGH = suministro comercial presente (salida del comparador del
    // circuito de respaldo); LOW = conmutado a la batería/respaldo de 5V.
    _conSuministro = digitalRead(PIN_RESPALDO_SENSE) == HIGH;
}

EstadoRespaldo revisarEstadoRespaldo() {
    EstadoRespaldo estado;
    const bool conSuministroAhora = digitalRead(PIN_RESPALDO_SENSE) == HIGH;

    if (conSuministroAhora != _conSuministro) {
        if (!conSuministroAhora) {
            estado.conmutoARespaldo = true;
            _avisoNivelBajoEnviado = false;
        } else {
            estado.recuperoSuministro = true;
        }
        _conSuministro = conSuministroAhora;
    }

    if (!_conSuministro && !_avisoNivelBajoEnviado) {
        const int nivelADC = analogRead(PIN_RESPALDO_BATERIA_ADC);
        if (nivelADC < UMBRAL_RESPALDO_BATERIA_ADC) {
            estado.nivelBajo = true;
            _avisoNivelBajoEnviado = true;
        }
    }

    return estado;
}

#else  // !RESPALDO_INSTALADO

void inicializarRespaldo() {}

EstadoRespaldo revisarEstadoRespaldo() {
    return EstadoRespaldo{};
}

#endif
