#include <stdio.h>
#include "geomap.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define GEO_PI 3.14159265358979323846
#define GEO_MAX_LAT 85.0511287798 /* предел проекции Web Mercator */

#define GEO_E 0.0818191908426215       /* первый эксцентриситет эллипсоида WGS84 */
#define GEO_MAX_LAT_ELL 85.0840591556  /* предел эллипсоидального Меркатора (квадратный мир) */

static double clamp_lat_p(double lat, int proj) {
    double lim = proj == GEO_PROJ_ELLIPSOID ? GEO_MAX_LAT_ELL : GEO_MAX_LAT;
    if (lat > lim)
        return lim;
    if (lat < -lim)
        return -lim;
    return lat;
}

double geo_world_size(int z) {
    return 256.0 * (double)(1L << z);
}

double geo_world_xp(double lon, int z, int proj) {
    (void)proj; /* долгота одинакова в обеих проекциях */
    return (lon + 180.0) / 360.0 * geo_world_size(z);
}

/* Вертикальная координата Меркатора в долях радиуса: на сфере это
 * atanh(sin(phi)); на эллипсоиде из неё вычитается e*atanh(e*sin(phi)). */
static double merc_t(double lat_deg, int proj) {
    double s = sin(clamp_lat_p(lat_deg, proj) * GEO_PI / 180.0);
    double t = 0.5 * log((1.0 + s) / (1.0 - s));
    if (proj == GEO_PROJ_ELLIPSOID)
        t -= GEO_E * 0.5 * log((1.0 + GEO_E * s) / (1.0 - GEO_E * s));
    return t;
}

double geo_world_yp(double lat, int z, int proj) {
    return (0.5 - merc_t(lat, proj) / (2.0 * GEO_PI)) * geo_world_size(z);
}

double geo_lon_from_xp(double x, int z, int proj) {
    (void)proj;
    return x / geo_world_size(z) * 360.0 - 180.0;
}

double geo_lat_from_yp(double y, int z, int proj) {
    double t = GEO_PI - 2.0 * GEO_PI * y / geo_world_size(z); /* = merc_t(lat) */
    double phi = atan(sinh(t)); /* на сфере это уже ответ */
    if (proj == GEO_PROJ_ELLIPSOID) {
        /* обратная задача на эллипсоиде решается итерациями (сходятся за 4-5 шагов) */
        for (int i = 0; i < 12; i++) {
            double es = GEO_E * sin(phi);
            double next = 2.0 * atan(exp(t) * pow((1.0 + es) / (1.0 - es), GEO_E / 2.0)) - GEO_PI / 2.0;
            if (fabs(next - phi) < 1e-13) {
                phi = next;
                break;
            }
            phi = next;
        }
    }
    return phi * 180.0 / GEO_PI;
}

/* Сферические варианты - прежний интерфейс. */
double geo_world_x(double lon, int z) { return geo_world_xp(lon, z, GEO_PROJ_SPHERICAL); }
double geo_world_y(double lat, int z) { return geo_world_yp(lat, z, GEO_PROJ_SPHERICAL); }
double geo_lon_from_x(double x, int z) { return geo_lon_from_xp(x, z, GEO_PROJ_SPHERICAL); }
double geo_lat_from_y(double y, int z) { return geo_lat_from_yp(y, z, GEO_PROJ_SPHERICAL); }

double geo_bearing_deg(double lat1, double lon1, double lat2, double lon2) {
    double p1 = lat1 * GEO_PI / 180.0, p2 = lat2 * GEO_PI / 180.0;
    double dl = (lon2 - lon1) * GEO_PI / 180.0;
    double y = sin(dl) * cos(p2);
    double x = cos(p1) * sin(p2) - sin(p1) * cos(p2) * cos(dl);
    double deg = atan2(y, x) * 180.0 / GEO_PI;
    deg = fmod(deg + 360.0, 360.0);
    return deg;
}

double geo_distance_km(double lat1, double lon1, double lat2, double lon2) {
    double p1 = lat1 * GEO_PI / 180.0, p2 = lat2 * GEO_PI / 180.0;
    double dp = p2 - p1, dl = (lon2 - lon1) * GEO_PI / 180.0;
    double a = sin(dp / 2) * sin(dp / 2) + cos(p1) * cos(p2) * sin(dl / 2) * sin(dl / 2);
    return 2.0 * 6371.0 * asin(sqrt(a < 1.0 ? a : 1.0));
}

int geo_parse_coord(const char *s, double *out) {
    if (!s)
        return 0;
    while (*s == ' ')
        s++;
    if (!*s)
        return 0;
    int negative = 0;
    char c = (char)toupper((unsigned char)*s);
    if (c == 'N' || c == 'S' || c == 'E' || c == 'W') {
        negative = (c == 'S' || c == 'W');
        s++;
        while (*s == ' ')
            s++;
    }
    if (!*s)
        return 0;
    char *end = NULL;
    double v = strtod(s, &end);
    if (end == s)
        return 0;
    *out = negative ? -v : v;
    return 1;
}

void geo_update_heading(MapAircraft *a) {
    a->has_heading = 0;
    a->heading = 0.0;
    if (a->n_trail < 2)
        return;
    double lat2 = a->trail_lat[a->n_trail - 1];
    double lon2 = a->trail_lon[a->n_trail - 1];
    for (int k = a->n_trail - 2; k >= 0; k--) {
        if (geo_distance_km(a->trail_lat[k], a->trail_lon[k], lat2, lon2) >= 0.5) {
            a->heading = geo_bearing_deg(a->trail_lat[k], a->trail_lon[k], lat2, lon2);
            a->has_heading = 1;
            return;
        }
    }
}

void geo_fit_view_p(const MapAircraft *list, int n, int w, int h, int margin, int proj,
                    double *clat, double *clon, int *zoom) {
    int have = 0;
    double minlat = 0, maxlat = 0, minlon = 0, maxlon = 0;
    for (int i = 0; i < n; i++) {
        for (int k = 0; k <= list[i].n_trail; k++) {
            double la = k < list[i].n_trail ? list[i].trail_lat[k] : list[i].lat;
            double lo = k < list[i].n_trail ? list[i].trail_lon[k] : list[i].lon;
            if (!have) {
                minlat = maxlat = la;
                minlon = maxlon = lo;
                have = 1;
            } else {
                if (la < minlat) minlat = la;
                if (la > maxlat) maxlat = la;
                if (lo < minlon) minlon = lo;
                if (lo > maxlon) maxlon = lo;
            }
        }
    }
    if (!have) {
        *clat = 50.0;
        *clon = 30.0;
        *zoom = 4;
        return;
    }

    int aw = w - 2 * margin, ah = h - 2 * margin;
    if (aw < 50) aw = 50;
    if (ah < 50) ah = 50;

    *zoom = 9;
    if (maxlat - minlat > 1e-4 || maxlon - minlon > 1e-4) {
        int z;
        for (z = 14; z > 2; z--) {
            double dx = geo_world_xp(maxlon, z, proj) - geo_world_xp(minlon, z, proj);
            double dy = geo_world_yp(minlat, z, proj) - geo_world_yp(maxlat, z, proj);
            if (dx <= aw && dy <= ah)
                break;
        }
        *zoom = z;
    }

    /* центр - середина ограничивающего прямоугольника в меркаторовых
     * (а не в градусных) координатах, иначе на больших масштабах
     * картинка съезжает по широте */
    int z = *zoom;
    double cx = (geo_world_xp(minlon, z, proj) + geo_world_xp(maxlon, z, proj)) / 2.0;
    double cy = (geo_world_yp(minlat, z, proj) + geo_world_yp(maxlat, z, proj)) / 2.0;
    *clon = geo_lon_from_xp(cx, z, proj);
    *clat = geo_lat_from_yp(cy, z, proj);
}

void geo_fit_view(const MapAircraft *list, int n, int w, int h, int margin,
                  double *clat, double *clon, int *zoom) {
    geo_fit_view_p(list, n, w, h, margin, GEO_PROJ_SPHERICAL, clat, clon, zoom);
}


/* дни от 1970-01-01 по гражданской дате (алгоритм Говарда Хиннанта) и обратно */
static long days_from_civil(long y, int m, int d) {
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    long yoe = y - era * 400;
    long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

long geo_time_parse(const char *s) {
    int Y, Mo, D, h, mi, se;
    if (!s || sscanf(s, "%d-%d-%d %d:%d:%d", &Y, &Mo, &D, &h, &mi, &se) != 6)
        return 0;
    if (Mo < 1 || Mo > 12 || D < 1 || D > 31 || h < 0 || h > 23 || mi < 0 || mi > 59 || se < 0 || se > 60)
        return 0;
    return days_from_civil(Y, Mo, D) * 86400L + h * 3600L + mi * 60L + se;
}

void geo_time_format(long t, char out[20]) {
    long days = t / 86400, rem = t % 86400;
    if (rem < 0) { rem += 86400; days--; }
    long z = days + 719468;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    long doe = z - era * 146097;
    long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long y = yoe + era * 400;
    long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long mp = (5 * doy + 2) / 153;
    int d = (int)(doy - (153 * mp + 2) / 5 + 1);
    int m = (int)(mp < 10 ? mp + 3 : mp - 9);
    y += m <= 2;
    char tmp[96];
    snprintf(tmp, sizeof tmp, "%04ld-%02d-%02d %02d:%02d:%02d", y, m, d, (int)(rem / 3600), (int)(rem % 3600 / 60), (int)(rem % 60));
    memcpy(out, tmp, 19);
    out[19] = 0;
}
