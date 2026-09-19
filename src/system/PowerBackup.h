#ifndef SYSTEM_POWER_BACKUP_H
#define SYSTEM_POWER_BACKUP_H

/**
 * HU-52 (backlog 54 HU): continuidad de medición ante corte de suministro
 * eléctrico. Sin `RESPALDO_INSTALADO` (ver config.h), este módulo no toca
 * ningún pin y siempre reporta "con suministro comercial, sin nivel bajo" —
 * mismo patrón que MC38Sensor para hardware opcional no instalado.
 *
 * Este backend de sensado NO ha sido validado contra el circuito real de
 * respaldo de 5V: el umbral de nivel bajo y el propio mapeo del ADC son un
 * punto de partida, no una autonomía medida (HU-52 escenario 3 lo exige
 * explícitamente: "la autonomía real del respaldo se mide y documenta en el
 * piloto/validación y no se fija por supuesto").
 */

struct EstadoRespaldo {
    /// El nodo acaba de conmutar de suministro comercial a respaldo.
    bool conmutoARespaldo = false;
    /// El nodo acaba de recuperar el suministro comercial.
    bool recuperoSuministro = false;
    /// Con respaldo activo, su nivel cayó bajo el umbral configurado — se
    /// notifica UNA vez por episodio (no en cada chequeo).
    bool nivelBajo = false;
};

/// Configura el pin de sensado. Llamar una vez en `setup()`.
void inicializarRespaldo();

/// Lee el estado actual y lo compara con el anterior. Debe llamarse
/// periódicamente (ver `INTERVALO_CHEQUEO_RESPALDO_MS`); cada transición se
/// reporta una sola vez, en el chequeo donde ocurre.
EstadoRespaldo revisarEstadoRespaldo();

#endif  // SYSTEM_POWER_BACKUP_H
