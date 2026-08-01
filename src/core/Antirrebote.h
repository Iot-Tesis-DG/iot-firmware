#ifndef CORE_ANTIRREBOTE_H
#define CORE_ANTIRREBOTE_H

#include <cstdint>

namespace core {

/**
 * Antirrebote del reed switch MC-38 y ventana de reporte, con el tiempo
 * inyectado en vez de leído de `millis()`.
 *
 * Esta es la lógica que HU-04 exige y la única del firmware donde un error se
 * traduce directamente en un dato falso de la tesis: si el antirrebote no
 * funciona, cada rebote mecánico del contacto se contabiliza como una apertura
 * de puerta. Con `millis()` incrustado no se podía comprobar; con el tiempo
 * como parámetro, una prueba en el host reproduce un tren de rebotes de 5 ms y
 * verifica que se cuenta una sola apertura.
 *
 * Convenciones:
 *   - `crudo == true`  → pin en HIGH → puerta ABIERTA.
 *   - Todos los instantes son milisegundos monótonos (base `millis()`).
 *   - Las restas son sin signo: correctas al desbordar a los ~49.7 días.
 */
class Antirrebote {
public:
    explicit Antirrebote(uint32_t debounceMs = 50) : _debounceMs(debounceMs) {}

    /// Fija el estado inicial sin aplicar antirrebote (arranque del nodo).
    void inicializar(bool crudo, uint32_t ahoraMs) {
        _estadoEstable = crudo;
        _ultimaMuestra = crudo;
        _muestraDesde = ahoraMs;
        _acumuladoMs = 0;
        _abiertaDesde = 0;
        _aperturaEnCurso = false;
        _huboApertura = false;
        if (crudo) {
            _abiertaDesde = ahoraMs;
            _aperturaEnCurso = true;
            _huboApertura = true;
        }
    }

    /// Procesa una muestra del pin. Devuelve true si el estado estable cambió.
    bool muestrear(bool crudo, uint32_t ahoraMs) {
        // El temporizador se reinicia cuando cambia la MUESTRA, no cuando la
        // muestra coincide con el estado estable: reiniciarlo en cada lectura
        // tranquila impedía que el umbral llegase a cumplirse nunca.
        if (crudo != _ultimaMuestra) {
            _ultimaMuestra = crudo;
            _muestraDesde = ahoraMs;
            return false;
        }
        if (crudo == _estadoEstable) return false;
        if ((uint32_t)(ahoraMs - _muestraDesde) < _debounceMs) return false;

        _estadoEstable = crudo;
        if (crudo) {
            _abiertaDesde = ahoraMs;
            _aperturaEnCurso = true;
            _huboApertura = true;
        } else if (_aperturaEnCurso) {
            _acumuladoMs += (uint32_t)(ahoraMs - _abiertaDesde);
            _aperturaEnCurso = false;
        }
        return true;
    }

    bool estaAbierta() const { return _estadoEstable; }

    /// ¿Hubo alguna apertura desde `cerrarVentana()`?
    bool huboApertura() const { return _huboApertura; }

    /// Milisegundos con la puerta abierta acumulados en la ventana actual,
    /// incluida la apertura todavía en curso.
    uint32_t duracionMs(uint32_t ahoraMs) const {
        uint32_t total = _acumuladoMs;
        if (_aperturaEnCurso) total += (uint32_t)(ahoraMs - _abiertaDesde);
        return total;
    }

    uint32_t duracionSegundos(uint32_t ahoraMs) const { return duracionMs(ahoraMs) / 1000; }

    /// Cierra la ventana de reporte. Si la puerta sigue abierta, la ventana
    /// siguiente arranca contando desde ya y mantiene el indicador: la puerta
    /// está abierta *también* en esa ventana.
    void cerrarVentana(uint32_t ahoraMs) {
        _acumuladoMs = 0;
        if (_aperturaEnCurso) {
            _abiertaDesde = ahoraMs;
            _huboApertura = true;
        } else {
            _huboApertura = false;
        }
    }

private:
    uint32_t _debounceMs;
    bool _estadoEstable = false;
    bool _ultimaMuestra = false;
    uint32_t _muestraDesde = 0;

    bool _huboApertura = false;
    uint32_t _acumuladoMs = 0;
    uint32_t _abiertaDesde = 0;
    // Bandera explícita en vez de usar `_abiertaDesde == 0` como centinela.
    // `millis()` vale 0 durante el primer milisegundo tras el arranque y vuelve
    // a valer 0 al desbordar cada ~49.7 días: en ambos instantes una apertura
    // en curso se daba por cerrada y su duración se perdía. Una prueba del host
    // (`test_arranque_con_la_puerta_abierta`) lo destapó.
    bool _aperturaEnCurso = false;
};

}  // namespace core

#endif  // CORE_ANTIRREBOTE_H
