#include "Reloj.h"

#include <cstdio>
#include <cstring>

namespace core {

// =========================================================================
// Conversión civil <-> días desde la época, calculada explícitamente.
//
// No se usa `mktime()`: interpreta el `struct tm` como hora LOCAL y aplica la
// zona horaria y el horario de verano del sistema. El código anterior lo usaba
// para convertir la hora de compilación —que es lo que se emite como timestamp
// UTC cuando NTP falla— así que el resultado dependía del huso configurado en
// ese momento en el ESP32. Y `gmtime()` devuelve un `struct tm` estático
// compartido: se llamaba desde el Core 0 mientras el Core 1 podía estar en NTP.
//
// El algoritmo es el de Howard Hinnant (days_from_civil), válido para todo el
// rango del calendario gregoriano proléptico y sin estado global.
// =========================================================================

static long diasDesdeEpoca(int anio, unsigned mes, unsigned dia) {
    anio -= mes <= 2;
    const long era = (anio >= 0 ? anio : anio - 399) / 400;
    const unsigned yoe = (unsigned)(anio - era * 400);              // [0, 399]
    const int mesDesplazado = (int)mes + (mes > 2 ? -3 : 9);
    const unsigned doy = (unsigned)((153 * mesDesplazado + 2) / 5) + dia - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;     // [0, 146096]
    return era * 146097L + (long)doe - 719468L;
}

static void civilDesdeDias(long dias, int& anio, unsigned& mes, unsigned& dia) {
    dias += 719468L;
    const long era = (dias >= 0 ? dias : dias - 146096) / 146097;
    const unsigned doe = (unsigned)(dias - era * 146097);           // [0, 146096]
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const long y = (long)yoe + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    dia = doy - (153 * mp + 2) / 5 + 1;
    mes = (unsigned)((int)mp + (mp < 10 ? 3 : -9));
    anio = (int)(y + (mes <= 2 ? 1 : 0));
}

std::string formatearISO8601(time_t epoch) {
    long segundos = (long)epoch;
    long dias = segundos / 86400;
    long resto = segundos % 86400;
    if (resto < 0) { resto += 86400; dias -= 1; }

    int anio;
    unsigned mes, dia;
    civilDesdeDias(dias, anio, mes, dia);

    char buf[32];
    snprintf(buf, sizeof(buf), "%04d-%02u-%02uT%02ld:%02ld:%02ldZ",
             anio, mes, dia, resto / 3600, (resto % 3600) / 60, resto % 60);
    return std::string(buf);
}

time_t epocaDeCompilacion(const char* fecha, const char* hora) {
    if (fecha == nullptr || hora == nullptr) return 0;

    static const char* meses[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

    char nombreMes[16] = {0};
    int dia = 0, anio = 0, h = 0, m = 0, s = 0;

    // %15s acota la escritura en `nombreMes`: con `%s` a secas, una macro
    // `__DATE__` inesperadamente larga desbordaba el buffer de pila.
    if (sscanf(fecha, "%15s %d %d", nombreMes, &dia, &anio) != 3) return 0;
    if (sscanf(hora, "%d:%d:%d", &h, &m, &s) != 3) return 0;

    int mes = -1;
    for (int i = 0; i < 12; i++) {
        if (strcmp(nombreMes, meses[i]) == 0) { mes = i + 1; break; }
    }
    if (mes < 0) return 0;
    if (dia < 1 || dia > 31 || anio < 1970) return 0;
    if (h < 0 || h > 23 || m < 0 || m > 59 || s < 0 || s > 60) return 0;

    const long dias = diasDesdeEpoca(anio, (unsigned)mes, (unsigned)dia);
    return (time_t)(dias * 86400L + h * 3600L + m * 60L + s);
}

}  // namespace core
