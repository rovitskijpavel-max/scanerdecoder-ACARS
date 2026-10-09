cmake_minimum_required(VERSION 3.10)
project(acarsdecoder C)

set(CMAKE_C_STANDARD 11)
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

include_directories(include)

set(CORE_SOURCES
    src/demod.c
    src/framesync.c
    src/wavio.c
    src/ifdemod.c
    src/squelch.c
    src/bitsync.c
    src/coherent_am.c
    src/mskpll.c
    src/history_log.c
    src/fileio.c
    src/msg_sink.c
    src/arinc622.c
    src/aircraft_db.c
    src/ooi_parse.c
    src/airport_db.c
    src/cfdiu_fault.c
    src/asu_telemetry.c
    src/acars_report.c
    src/journal_export.c
    src/aircraft_report.c
    src/metar.c
    src/geomap.c
)

add_executable(acarsdecoder src/main.c ${CORE_SOURCES})
add_executable(acars_selftest src/selftest.c src/demod.c src/framesync.c)

if (WIN32)
    target_sources(acarsdecoder PRIVATE src/audio_win.c src/gui_win.c src/map_view.c src/memory_view.c src/ar5000.c resources/acars.rc)
    target_include_directories(acarsdecoder PRIVATE resources)
    # winmm - классическое Win32 Multimedia API (waveIn*): работает от
    # Windows 95/98/2000/XP до 10/11 без изменений, в отличие от WASAPI
    # (Vista+), которого раньше использовался здесь. comdlg32 - диалог
    # "Открыть файл" для графического интерфейса. _WIN32_WINNT=0x0501
    # (Windows XP) ограничивает видимые объявления в заголовках именно
    # XP-совместимым подмножеством - страховка от случайного использования
    # более новых API при дальнейшей доработке.
    target_link_libraries(acarsdecoder winmm comdlg32 gdi32 comctl32)
    target_compile_definitions(acarsdecoder PRIVATE _WIN32_WINNT=0x0501 WINVER=0x0501)
    if (NOT MSVC)
        # Статическая линковка под MinGW: .exe не зависит от DLL
        # MSYS2/MinGW во время выполнения - переносится на флешке на
        # любой Windows-компьютер без установленного MSYS2.
        target_link_options(acarsdecoder PRIVATE -static)
    else()
        # Для реального запуска на живой XP через MSVC дополнительно
        # нужен набор инструментов "v141_xp"/"v142_xp" (устанавливается
        # отдельным компонентом в Visual Studio Installer) - обычный
        # современный MSVC-рантайм на XP не запустится, даже если код
        # сам по себе XP-совместим. См. README.
    endif()
endif()

target_link_libraries(acarsdecoder m)
target_link_libraries(acars_selftest m)

if (NOT WIN32)
    # pthread - потокобезопасная запись истории (history_log.c) на Linux/macOS.
    find_package(Threads REQUIRED)
    target_link_libraries(acarsdecoder Threads::Threads)
endif()

if (MSVC)
    target_compile_options(acarsdecoder PRIVATE /W4)
else()
    target_compile_options(acarsdecoder PRIVATE -O2 -Wall)
    target_compile_options(acars_selftest PRIVATE -O2 -Wall)
endif()
