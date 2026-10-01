# J2ME Emulator

Эмулятор мобильных Java-приложений (MIDP2 / CLDC 1.1) с поддержкой SDL2 и Libretro.

Реализует собственную JVM с интерпретатором байткода, MIDP2 API (Graphics, Display, Game Canvas, RMS, Media, M3G) и предназначен для запуска J2ME игр и приложений на десктопных платформах и ретро-консолях.

Веб версия (устарела, просто показ возможностей) https://corax89.github.io/nojme/

## Возможности

- **Полная JVM** — интерпретатор всех 256 опкодов Java байткода, загрузчик классов, сборщик мусора, потоки
- **MIDP2 API** — Graphics, Display, Font, Image, Form, List, TextBox, Alert, GameCanvas, Sprite, TiledLayer, LayerManager
- **M3G (JSR-184)** — Mobile 3D Graphics: загрузка .m3g файлов, рендеринг 3D-сцен
- **RMS** — Record Management System с сохранением на диск
- **Медиа** — воспроизведение MIDI-мелодий и WAV/PCM-звуков
- **Libretro Core** — запуск через RetroArch (целевые платформы: M17, Windows, Linux)
- **Headless-режим** — выполнение без дисплея для тестирования и CI
- **switchui** - версия для нинтендо свитч

## Сборка

### Требования

- GCC (C11) или MinGW
- Make
- SDL2 (только для standalone-сборки)
- pthreads

### Standalone-приложение (SDL2)

```bash
make app
./bin/j2me-emulator game.jar
```

### Headless-режим (без SDL2)

```bash
make headless
./bin/j2me-headless game.jar
```

### Libretro Core

```bash
# Linux
make platform=linux

# Windows (MinGW native)
make

# Windows (cross-compile с Linux)
make CROSS_COMPILE=x86_64-w64-mingw32-

# M17 (ARM 32-bit)
make platform=m17 CROSS_COMPILE=/opt/m17-toolchain/usr/bin/arm-buildroot-linux-gnueabihf-
```

## Использование

```bash
# Запуск с автодетекцией MIDlet-класса из манифеста
./bin/j2me-emulator game.jar

# Указание класса вручную
./bin/j2me-emulator game.jar com.example.MyMIDlet

# Настройка экрана и фуллскрин
./bin/j2me-emulator -w 320 -h 480 -s 2 -f game.jar

# Размер кучи
./bin/j2me-emulator --heap-size 32 game.jar

# Headless (для тестирования)
./bin/j2me-headless game.jar
```

## Структура проекта

```
src/
├── jvm/            # Ядро JVM: загрузчик классов, интерпретатор, heap, GC, потоки
├── midp/           # MIDP2 API: graphics, display, form, media, mobile3d, rms
├── sdl/            # SDL2 бэкенд и headless-реализация
├── libretro/       # Libretro core
├── midi/           # MIDI-проигрыватель
├── utils/          # Утилиты: miniz (zlib), stb_image, helpers
└── main.c          # Точка входа
include/            # Заголовочные файлы
tests/              # Тестовые JAR-файлы
```

## Тестирование

```bash
make headless
for jar in tests/*.jar; do
    echo "=== $(basename $jar) ==="
    ./bin/j2me-headless "$jar" 2>&1 | tail -5
    echo
done
```

## Целевые платформы

| Платформа         | Формат      | Архитектура |
|-------------------|-------------|-------------|
| Linux             | `.so` / ELF | x86_64, ARM |
| Windows           | `.dll`      | x86_64      |
| M17               | `.so`       | ARM Cortex-A7 |
| RetroArch (общий) | core        | любая       |

## Лицензия

См. LICENSE файл.
