#ifndef GEOMAP_H
#define GEOMAP_H

/*
 * Переносимая (без Windows) математика для встроенной карты: проекция
 * Web Mercator (та же, что у тайлов OpenStreetMap/Google/Flightradar),
 * курс по двум точкам, подгонка масштаба под набор бортов, разбор
 * координат из телеметрии ASU/AFL. Вынесено отдельно от окна карты
 * (map_view.c), чтобы это можно было проверить обычными тестами, не
 * запуская Windows.
 */

#define MAP_MAX_TRAIL 48

/* Один борт на карте: последняя позиция, след (прошлые позиции по
 * порядку получения) и то, что известно о нём из телеметрии. Строки -
 * UTF-8. */
typedef struct {
    char reg[24];     /* регистрация без ведущей точки, например "RA73104" */
    char hex[8];      /* код Mode S (ICAO24), если известен по базе бортов, иначе пусто */
    char type[16];    /* код типа (A320, B738...) из базы бортов, иначе пусто - по нему рисуется силуэт */
    char model[40];   /* модель из базы бортов ("A320-214"), иначе пусто - для поиска фото по модели */
    char operator_name[80]; /* компания из базы бортов ("Aeroflot..."), иначе пусто - для поиска фото по компании */
    char time[32];    /* время ПОСЛЕДНЕГО сообщения с координатами */
    char info[320];   /* тип/оператор из aircraft_db.csv, если база загружена */
    char alt[16], fuel[16], eta[16];
    char flight[8];   /* номер рейса из заголовка сообщения ("SU1563"), иначе пусто */
    char dep[5], dest[5]; /* ICAO аэропортов вылета/назначения из служебных отчётов (AFL/SDM), иначе пусто */
    double lat, lon;  /* последняя позиция, градусы (S/W отрицательные) */
    int n_trail;
    double trail_lat[MAP_MAX_TRAIL], trail_lon[MAP_MAX_TRAIL];
    /* данные каждой точки следа (то, что было в сообщении с этой позицией); пусто - в сообщении не было */
    char trail_time[MAP_MAX_TRAIL][32];
    char trail_label[MAP_MAX_TRAIL][4];     /* метка ACARS сообщения этой точки */
    char trail_text[MAP_MAX_TRAIL][400];    /* текст сообщения этой точки (может быть обрезан) */
    char trail_alt[MAP_MAX_TRAIL][16], trail_fuel[MAP_MAX_TRAIL][16], trail_eta[MAP_MAX_TRAIL][16];
    int trail_seq[MAP_MAX_TRAIL];   /* номер сообщения борта (MSN, 0-99) или -1 */
    long trail_recv[MAP_MAX_TRAIL]; /* время приёма, секунд (для сравнения близких по времени) */
    char trail_flight[8]; /* рейс, которому принадлежит след (след начинается заново при смене рейса) */
    int has_heading;  /* 1, если курс удалось вычислить по следу */
    double heading;   /* градусы по часовой от севера, 0..360 */
} MapAircraft;

/* Размер "мира" в пикселях на уровне масштаба z (256 * 2^z). */
double geo_world_size(int z);

/* Широта/долгота -> пиксельные координаты мира на уровне z
 * (начало - левый верхний угол мира, y растёт вниз - как у тайлов). */
double geo_world_x(double lon, int z);
double geo_world_y(double lat, int z);

/* Обратное преобразование. */
double geo_lon_from_x(double x, int z);
double geo_lat_from_y(double y, int z);

/* --- Проекции --------------------------------------------------------
 * Тайлы бывают в двух проекциях Меркатора, и перепутать их нельзя - разница
 * по широте доходит до десятков километров:
 *   GEO_PROJ_SPHERICAL - Меркатор на сферу (EPSG:3857): Google, OSM, Bing,
 *                        CARTO и т.п.;
 *   GEO_PROJ_ELLIPSOID - Меркатор на эллипсоид WGS84 (EPSG:3395): Яндекс.Карты,
 *                        Космоснимки (в SAS.Planet это projection=2).
 * Долгота в обеих проекциях считается одинаково, различается широта.
 * Функции без суффикса _p выше - это сферическая проекция. */
#define GEO_PROJ_SPHERICAL 0
#define GEO_PROJ_ELLIPSOID 1

double geo_world_xp(double lon, int z, int proj);
double geo_world_yp(double lat, int z, int proj);
double geo_lon_from_xp(double x, int z, int proj);
double geo_lat_from_yp(double y, int z, int proj);

/* Начальный курс (азимут) от точки 1 к точке 2, градусы 0..360. */
double geo_bearing_deg(double lat1, double lon1, double lat2, double lon2);

/* Расстояние по большому кругу, км. */
double geo_distance_km(double lat1, double lon1, double lat2, double lon2);

/* Разбор координаты из телеметрии: "N 55.835", "S 12.345", "E 43.376",
 * "W 6.789" (буква направления, затем число) либо просто число со
 * знаком ("-12.5"). S и W дают отрицательное значение. Возвращает 1 при
 * успехе. */
int geo_parse_coord(const char *s, double *out);

/* Считает курс по следу: берёт последнюю позицию и ближайшую более
 * раннюю, отстоящую минимум на 0.5 км (на меньшем расстоянии курс
 * вычислять бессмысленно - это шум округления координат). Заполняет
 * has_heading/heading. */
void geo_update_heading(MapAircraft *a);

/* Подбирает центр и масштаб так, чтобы все борта (и их следы) влезли в
 * окно w x h пикселей с полями margin. Один борт или совпадающие точки
 * - масштаб 9. Нет бортов - вид по умолчанию (центр Европы). */
void geo_fit_view(const MapAircraft *list, int n, int w, int h, int margin,
                  double *clat, double *clon, int *zoom);

/* То же, но для выбранной проекции подложки. */
void geo_fit_view_p(const MapAircraft *list, int n, int w, int h, int margin, int proj,
                    double *clat, double *clon, int *zoom);

/* Время для проигрывания истории: "ГГГГ-ММ-ДД ЧЧ:ММ:СС" <-> секунды (местное время как есть, без часовых поясов).
 * geo_time_parse возвращает 0, если строка не разобрана. */
long geo_time_parse(const char *s);
void geo_time_format(long t, char out[20]);

#endif
