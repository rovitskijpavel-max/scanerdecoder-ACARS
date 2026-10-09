/* Проверка координат: проекции туда-обратно, знаки S/W, согласованность двух разборщиков, маршрут. */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "geomap.h"
#include "acars_report.h"
#include "asu_telemetry.h"
#include "aircraft_report.h"

static int fails;
#define CHECK(c, m) do { if (!(c)) { fails++; printf("FAIL: %s\n", m); } } while (0)

int main(void) {
    /* проекции: туда-обратно с точностью до сантиметров на всех масштабах */
    double lats[] = {-60, -33.9, 0, 41.2753, 55.9726, 59.8003, 70};
    double lons[] = {-170, -70.5, 0, 28.7519, 37.4146, 44.3455, 139.7, 179};
    for (int proj = 0; proj < 2; proj++)
        for (int z = 3; z <= 18; z++)
            for (unsigned i = 0; i < sizeof lats / sizeof *lats; i++)
                for (unsigned j = 0; j < sizeof lons / sizeof *lons; j++) {
                    double x = geo_world_xp(lons[j], z, proj), y = geo_world_yp(lats[i], z, proj);
                    double la = geo_lat_from_yp(y, z, proj), lo = geo_lon_from_xp(x, z, proj);
                    if (fabs(la - lats[i]) > 1e-6 || fabs(lo - lons[j]) > 1e-6) {
                        fails++;
                        printf("FAIL roundtrip proj=%d z=%d %.4f %.4f -> %.6f %.6f\n", proj, z, lats[i], lons[j], la, lo);
                    }
                }
    /* север - вверх (y растёт вниз), восток - вправо */
    CHECK(geo_world_yp(60, 8, 0) < geo_world_yp(50, 8, 0), "north is up");
    CHECK(geo_world_xp(40, 8, 0) > geo_world_xp(30, 8, 0), "east is right");

    /* знаки */
    double v;
    CHECK(geo_parse_coord("N 55.835", &v) && fabs(v - 55.835) < 1e-9, "N");
    CHECK(geo_parse_coord("S 12.345", &v) && fabs(v + 12.345) < 1e-9, "S");
    CHECK(geo_parse_coord("E 43.376", &v) && fabs(v - 43.376) < 1e-9, "E");
    CHECK(geo_parse_coord("W 6.789", &v) && fabs(v + 6.789) < 1e-9, "W");

    /* AFL-сообщение южного/западного полушария: оба разборщика должны дать отрицательные значения */
    const char *t = "M56ASU1603POS01AFL1603   /06061234UUEEULLI    \r\nFOB 100\r\nLATS 33.924\r\nLONW 18.601\r\nALT 36000\r\n";
    AcarsSummary sm;
    CHECK(acars_report_summary("27", t, &sm), "summary parsed");
    printf("S/W summary: has_pos=%d lat=%.3f lon=%.3f\n", sm.has_pos, sm.lat, sm.lon);
    CHECK(sm.has_pos && fabs(sm.lat + 33.924) < 1e-6 && fabs(sm.lon + 18.601) < 1e-6, "S/W summary signs");
    AsuTelemetry tel;
    if (asu_telemetry_parse(t, &tel) && tel.lat[0]) {
        double a, b;
        CHECK(geo_parse_coord(tel.lat, &a) && geo_parse_coord(tel.lon, &b) && fabs(a + 33.924) < 1e-6 && fabs(b + 18.601) < 1e-6,
              "S/W telemetry signs");
    } else { fails++; puts("FAIL telemetry S/W not parsed"); }

    /* реальный пример: LDO RA89064 у URWW (Гумрак) - позиция в 2-3 км от аэродрома */
    double d = geo_distance_km(48.796, 44.372, 48.7825, 44.3455);
    CHECK(d < 3.5, "LDO near URWW");

    /* маршрут из сообщений */
    ReportMsg m[2] = {
        {"2026-09-29 13:30:00", "26", "", "M56ASU1563ETA01AFL1563   /29101530UIIIUUEE    \r\nFOB 120\r\nLATN 58.373\r\nLONE 41.131\r\n"},
        {"2026-09-29 13:58:21", "27", "", "M56BSU1563POS01AFL1563   /29101858UIIIUUEE    \r\nFOB 118\r\nLATN 58.373\r\nLONE 41.131\r\n"},
    };
    char f[8], dep[5], dest[5];
    CHECK(aircraft_report_last_route(m, 2, f, dep, dest) && !strcmp(dep, "UIII") && !strcmp(dest, "UUEE") && !strcmp(f, "SU1563"), "last route");
    ReportFlight fl[4];
    CHECK(aircraft_report_flights(m, 2, fl, 4) == 1 && fl[0].count == 2, "flights aggregated");

    /* время для проигрывания истории: туда-обратно, разница в секундах */
    {
        const char *ts[] = {"2026-10-06 13:50:08", "2000-02-29 23:59:59", "2026-12-31 00:00:00", "2024-03-01 00:00:01"};
        for (unsigned i = 0; i < 4; i++) {
            char o[20];
            long t = geo_time_parse(ts[i]);
            geo_time_format(t, o);
            CHECK(t > 0 && !strcmp(o, ts[i]), "geo_time round trip");
        }
        CHECK(geo_time_parse("2026-10-06 14:00:08") - geo_time_parse("2026-10-06 13:50:08") == 600, "geo_time diff");
        CHECK(geo_time_parse("мусор") == 0, "geo_time garbage");
    }

    printf(fails ? "COORD TESTS FAILED: %d\n" : "coord tests OK\n", fails);
    return fails != 0;
}
