# Простой Makefile. Под Windows используйте MSYS2/MinGW-w64:
#   pacman -S mingw-w64-x86_64-gcc make
#   mingw32-make
# Под Linux (для тестирования на WAV-файлах, без живого приёма):
#   make

CC = gcc
CFLAGS = -O2 -Wall -Iinclude
LIBS = -lm

CORE_SRC = src/demod.c src/framesync.c src/wavio.c src/fileio.c src/ifdemod.c src/squelch.c src/bitsync.c src/coherent_am.c src/mskpll.c src/history_log.c src/msg_sink.c src/arinc622.c src/aircraft_db.c src/ooi_parse.c src/airport_db.c src/cfdiu_fault.c src/asu_telemetry.c src/acars_report.c src/journal_export.c src/aircraft_report.c src/metar.c src/geomap.c

ifeq ($(OS),Windows_NT)
    TARGET = acarsdecoder.exe
    SRC = src/main.c src/audio_win.c src/gui_win.c src/map_view.c src/memory_view.c src/ar5000.c $(CORE_SRC)
    RC_OBJ = resources/acars_res.o
    RC = windres
    # winmm - классическое Win32 Multimedia API (waveIn*), работает от
    # Windows 95/98/2000/XP до 10/11 без изменений - в отличие от
    # WASAPI, которого на XP попросту нет.
    # comdlg32 - диалог "Открыть файл"; gdi32 - шрифты и рисование окна
    # (GetStockObject, CreateFontW и т.п.) для графического интерфейса.
    LIBS += -lwinmm -lcomdlg32 -lgdi32 -lcomctl32
    # _WIN32_WINNT=0x0501 (Windows XP) ограничивает видимые объявления в
    # заголовках именно XP-совместимым подмножеством - страховка от
    # случайного использования более новых API при дальнейшей доработке.
    CFLAGS += -D_WIN32_WINNT=0x0501 -DWINVER=0x0501 -Iresources
    # Статическая линковка: получившийся .exe не будет зависеть от
    # DLL-библиотек MSYS2/MinGW (libgcc, libwinpthread и т.п.) - можно
    # скопировать на флешку и запустить на ЛЮБОМ Windows-компьютере,
    # даже без установленного MSYS2.
    LDFLAGS += -static
    # Версия подсистемы 5.1 - иначе у новых сборок binutils Windows XP отказывается запускать .exe
    # ("не является приложением Win32").
    LDFLAGS += -Wl,--major-os-version=5 -Wl,--minor-os-version=1 -Wl,--major-subsystem-version=5 -Wl,--minor-subsystem-version=1
else
    TARGET = acarsdecoder
    SRC = src/main.c $(CORE_SRC)
    RC_OBJ =
    # pthread - потокобезопасная запись истории (history_log.c) на Linux.
    LIBS += -lpthread
endif

all: $(TARGET) acars_selftest

$(TARGET): $(SRC) $(RC_OBJ)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(RC_OBJ) $(LIBS) $(LDFLAGS)

# Значок приложения (resources/acars.ico, встраивается через .rc) -
# правило актуально только под Windows (RC_OBJ пуст на Linux, эта
# цель там просто никогда не запрашивается сборкой $(TARGET)).
resources/acars_res.o: resources/acars.rc resources/acars.ico resources/resource.h
	$(RC) -I resources resources/acars.rc -O coff -o resources/acars_res.o

acars_selftest: src/selftest.c src/demod.c src/framesync.c
	$(CC) $(CFLAGS) -o $@ src/selftest.c src/demod.c src/framesync.c -lm

clean:
	rm -f acarsdecoder acarsdecoder.exe acars_selftest acars_selftest.exe test_signal.wav resources/acars_res.o

.PHONY: all clean
