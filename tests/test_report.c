/* Регрессионный тест расшифровки текстов (acars_report.c) на строках из
 * реального журнала. Сборка: см. README, раздел "Тесты расшифровки". */
#include <stdio.h>
#include <string.h>
#include "acars_report.h"
#include "units.h"

static int fails = 0, total = 0;

static void check(const char *label, const char *text, const char *must1, const char *must2) {
    char out[4096];
    total++;
    int r = acars_report_decode(label, text, out, sizeof out);
    int ok = r && (!must1 || strstr(out, must1)) && (!must2 || strstr(out, must2));
    if (!ok) {
        fails++;
        printf("FAIL [%s] ожидалось \"%s\" / \"%s\"\n--- вывод ---\n%s\n", label,
               must1 ? must1 : "", must2 ? must2 : "", out);
    }
}

static void check_none(const char *label, const char *text) {
    char out[4096];
    total++;
    if (acars_report_decode(label, text, out, sizeof out)) {
        fails++;
        printf("FAIL [%s] не должно расшифровываться, а вывод: %s\n", label, out);
    }
}

int main(void) {
    AcarsHeader h;
    total++; if (!acars_header_parse("M56ASU1603POS01AFL1603", &h) || strcmp(h.flight, "SU1603") || h.seq != 56) { fails++; puts("FAIL header 1"); }
    total++; if (!acars_header_parse("S02AU67380", &h) || strcmp(h.flight, "U67380")) { fails++; puts("FAIL header U6"); }
    total++; if (acars_header_parse("METAR UMMS 061030Z", &h)) { fails++; puts("FAIL header METAR"); }
    total++; if (acars_header_parse("00XS", &h)) { fails++; puts("FAIL header SQ"); }

    check("27", "M56ASU1603POS01AFL1603   /29151006UWWWUUEE    \r\nFUEL   69\r\nTEMP- 52\r\nWDIR19867\r\nWSPD  16\r\nLATN 55.835\r\nLONE 43.376\r\nETA1101\r\nTUR \r\nALT  35992\r\n\r\n",
          "позиционный отчёт", "198.7");
    check("27", "M56ASU1603POS01AFL1603   /29151006UWWWUUEE    \r\nALT  36000\r\n", "10973 м (36000 ft)", NULL);
    {
        char t[64]; int ft = 0;
        total++; if (!alt_ft_text(" 36000", t, sizeof t, &ft) || strcmp(t, "10973 м (36000 ft)") || ft != 36000) { fails++; printf("FAIL alt_ft_text: %s\n", t); }
        total++; if (alt_ft_text("----", t, sizeof t, NULL) || strcmp(t, "---- ft")) { fails++; printf("FAIL alt_ft_text junk: %s\n", t); }
        total++; if (alt_ft_text("", t, sizeof t, NULL) || t[0]) { fails++; puts("FAIL alt_ft_text empty"); }
        char pb[2048];
        total++; if (acars_report_point("27", "M15ASU1296POS01AFL1296   /29291131UUEEUSII    \r\nFUEL   57\r\nTEMP- 57\r\nWDIR20396\r\nWSPD  49\r\nLATN 57.133\r\nLONE 46.881\r\nETA1212\r\nTUR \r\nALT  34984\r\n", pb, sizeof pb) < 8
                 || !strstr(pb, "Топливо: 57") || !strstr(pb, "10663 м (34984 ft)") || !strstr(pb, "Турбулентность")) { fails++; puts("FAIL point"); }
        total++; if (acars_report_point("27", "garbage", pb, sizeof pb) != 0) { fails++; puts("FAIL point garbage"); }
        AcarsSummary sm;
        total++; if (!acars_report_summary("27", "M56ASU1603POS01AFL1603   /29151006UWWWUUEE    \r\nFUEL   69\r\nTEMP- 52\r\nWDIR19867\r\nWSPD  16\r\nLATN 55.835\r\nLONE 43.376\r\nETA1101\r\nTUR \r\nALT  35992\r\n", &sm)
                 || strcmp(sm.dep, "UWWW") || strcmp(sm.dest, "UUEE") || !sm.has_pos || sm.alt_ft != 35992 || strcmp(sm.eta, "11:01") || strcmp(sm.type, "POS") || strcmp(sm.airline, "Аэрофлот")) { fails++; puts("FAIL summary"); }
        total++; if (!acars_report_summary("16", "M53AFV5954102019,34000,1110, 106,N 58.108 E 42.206", &sm) || !sm.has_pos || sm.lat < 58.1 || sm.alt_ft != 34000) { fails++; puts("FAIL summary 16"); }
    }
    check("27", "M56ASU1603POS01AFL1603   /29151006UWWWUUEE    \r\nFUEL   69\r\nTEMP- 52\r\nWDIR19867\r\nWSPD  16\r\nLATN 55.835\r\nLONE 43.376\r\nETA1101\r\nTUR \r\nALT  35992\r\n",
          "-52 °C", "Аэрофлот, рейс №1603");
    check("26", "M91ASU0801ETA01AFL801    /29291101LTAIUWGG    \r\nFUEL   61\r\nTEMP\r\nWDIR16555\r\nWSPD  11\r\nLATN 55.828\r\nLONE 43.954\r\nETA1111\r\nTUR \r\nALT   6849\r\n",
          "температура за бортом: нет данных", "рейс №801");
    check("12", "M35ASU2130OFF01AFL2130   /06061125UUEELTFM\r\n112500\r\nFOB  187\r\nLATN 55.984\r\nLONE 37.408\r\nETA1627\r\n",
          "взлёт", "11:25:00");
    check("14", "M12AFV6176INN01SDM6176   /05051658UUWWULLI\r\n165525\r\nFOB   40\r\nLATN 59.807\r\nLONE 30.274\r\n",
          "прибытие на гейт", "16:55:25");
    check("39", "M35AFV6117SFI01SDM6117   /06060846ULLIURWW\r\nDEPSULLI\r\nDESTURWW\r\nFUEL   32\r\nOUTT060753\r\nOFFT061414\r\nONNT083215\r\nINNT084239\r\nOIBT023500\r\nFLTT021800\r\nEONT------\r\nEOFT052107\r\nFUBT\r\nFBOU0025\r\nFBOF0001\r\nFBON0025\r\nFBIN0025\r\nWEI",
          "OFF - взлёт: 06:14:14", "время в блоках: 02:35:00");
    check("39", "M00AFV0000APS01----------/  030635--------\r\nDEPS----\r\nDEST----\r\nFUEL   63\r\nALTF    212\r\nARUN0000\r\n",
          "не передан", "номер рейса не задан");
    check("15", "M20AFV0000RTG01----------/  031340--------\r\n------134036\r\nFOB-----\r\nLATN 55.994\r\nLONE 37.428\r\n",
          "13:40:36", "нет данных");
    check("16", "M53AFV5954102019,34000,1110, 106,N 58.108 E 42.206", "10:20:19", "FL340");
    check("16", "M39AXX2168//104204,28002,1112,----.-,MMMM.MMM MMMM.MMM,", "координаты не получены", "FL280");
    check("Q0", "S21AFV6386", "Проверка канала", "Россия");
    check("SA", "S92ASU16420EV105719V", "10:57:19", "EV");
    check("5U", "M99APQBRU8UMMS-SA", "METAR", "UMMS");
    check("SQ", "00XS", "squitter", NULL);
    check("A4", "/ALMZTST.FS2/FSM 1037 260929 ULLI\r\nSU1563 TIS REJECTED\r\nERROR IN MESSAGE\r\nREVERT TO VOICE PROCEDURES\r\n",
          "ОТКЛОНИЛА", "2026-09-29");
    check("A9", "/LEMZTST.TI2/UUWW ARR ATIS D\r\n1030Z VNUKOVO ATIS ARR INFO D 10:30\r\n", "ATIS на прибытие", "литера D");
    check("A9", "/NITATST.TI2/UWWW ENR ATIS\r\n1030Z DEMAND SIGMET UWWW NIL\r\n", "по маршруту", "UWWW");
    check("C1", ".DMEASS7 291026\r\nAGM\r\nAN RA73335/MA 584A\r\n- \r\nSA 29/08:02\r\n", "DMEASS7", "RA73335");
    check("B9", "M21ASU1269/UUWW.TI2/040UUWWAF4FA", "Запрос ATIS", "UUWW");
    check_none("_\x7f", "");
    check_none("3M", "METAR UMMS 061030Z\r\n27005G08MPS 9999 BKN021\r\n15/11 Q1013 NOSIG");
    check_none("H1", "");

    /* устойчивость: обрезанные и мусорные тексты не должны падать */
    const char *junk[] = {"M56ASU1603POS01AFL1603   /2915", "M56ASU1603POS01AFL1603   /29151006UWWWUU", "M99A", "/", ".", "/A.B/", "M56ASU1603POS01AFL1603   /29151006UWWWUUEE\r\nLATN", "M56ASU1603POS01AFL1603   /29151006UWWWUUEE\r\nWDIR", "\r\n\r\n", NULL};
    for (int i = 0; junk[i]; i++) { char o[256]; acars_report_decode("27", junk[i], o, sizeof o); acars_report_decode("16", junk[i], o, 16); }
    total++;
    printf("%s: %d проверок, %d провалов\n", fails ? "ПРОВАЛ" : "ВСЁ ПРОШЛО", total, fails);
    return fails != 0;
}
