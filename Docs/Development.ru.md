# Команды сборки и проверки LocalScribe

Корневой `Makefile` предоставляет короткие команды для всех сценариев из
`Scripts/`. Он не дублирует логику сборки и тестов: каждая цель вызывает
соответствующий shell-скрипт, поэтому прямой запуск скриптов остаётся
равнозначным.

Все команды ниже запускаются из корня репозитория. Полный список с краткими
описаниями можно вывести так:

```sh
make help
```

## Сборка приложения

Сборка оптимизированного app bundle по умолчанию:

```sh
make build
```

Явно выбрать конфигурацию можно отдельной целью:

```sh
make build-debug
make build-release
```

Обе команды создают и ad hoc-подписывают `Build/LocalScribe.app`. Debug-сборка
подходит для локальной разработки, release-сборка — для последующей упаковки.

## Проверки и тесты

Основная проверка перед ревью или публикацией:

```sh
make verify
```

Это обёртка над `Scripts/verify-mvp.sh`: она проверяет границы проекта, запускает
Core-тесты, автономные Swift-проверки, короткий soak-тест и создаёт debug bundle.
Swift XCTest выполняется, когда выбран полный Xcode. Команда `make check`
является алиасом `make verify`.

Отдельные части можно запускать независимо:

| Команда | Что выполняется |
|---|---|
| `make check-scope` | Проверка macOS-only и offline-границ проекта |
| `make test-core` | C++20 contract-тесты переносимого ядра |
| `make check-swift` | Автономные проверки Swift-инвариантов |
| `make test-swift` | Swift XCTest; требуется полный Xcode |
| `make test` | Последовательно все три команды тестов и Swift-проверок |

Чтобы полный MVP-gate завершался ошибкой, если XCTest недоступен, задайте:

```sh
make verify LOCALSCRIBE_REQUIRE_SWIFT_TESTS=1
```

При нестандартном SDK его путь можно передать целям сборки и Swift-проверок:

```sh
make verify LOCALSCRIBE_SDKROOT=/path/to/MacOSX.sdk
```

## Soak-тесты

Для быстрой локальной проверки используйте короткий детерминированный режим:

```sh
make soak-smoke
```

Ускоренная симуляция двух часов запускается так:

```sh
make soak-accelerated
```

Полный real-time запуск занимает два часа:

```sh
make soak
```

Произвольные аргументы `run-core-soak.sh` передаются через `SOAK_ARGS`. Например,
эта команда симулирует 30 минут с ускорением 100x и сохраняет журнал по явно
заданному пути:

```sh
make soak SOAK_ARGS="--duration-seconds 1800 --speed 100 --journal /tmp/localscribe-soak.sqlite"
```

Компилятор C++ для soak-runner можно переопределить переменной `CXX`:

```sh
make soak-smoke CXX=/path/to/clang++
```

Полный сценарий и критерии прохождения описаны в
[Two-Hour Soak Test](Testing/Two-Hour-Soak-Test.md).

## Проверка реального Whisper ASR

Для standalone-проверки укажите локальную модель и WAV-файл:

```sh
make whisper-smoke \
    MODEL=/path/to/ggml-base.bin \
    WAV=/path/to/speech.wav
```

`MODEL` и `WAV` можно заменить уже используемыми проектом именами переменных
окружения:

```sh
LOCALSCRIBE_MODEL_PATH=/path/to/ggml-base.bin \
LOCALSCRIBE_WAV_PATH=/path/to/speech.wav \
make whisper-smoke
```

Те же переменные подключают проверку реального ASR к полному gate:

```sh
make verify \
    LOCALSCRIBE_MODEL_PATH=/path/to/ggml-base.bin \
    LOCALSCRIBE_WAV_PATH=/path/to/speech.wav
```

Smoke-проверка принимает little-endian PCM16 или Float32 WAV. Модели и
аудиозаписи нельзя добавлять в репозиторий.

## DMG и GitHub pre-release

Упаковать уже существующий `Build/LocalScribe.app` без повторной сборки:

```sh
make package
```

Если DMG и checksum для текущей версии уже находятся в `dist/`, упаковщик по
умолчанию остановится. Явно заменить эти артефакты можно так:

```sh
make package-overwrite
```

Полный технический pre-release — проверки, новая release-сборка и DMG:

```sh
make github-release
```

Доступны варианты для повторной локальной упаковки:

```sh
make github-release-overwrite
make github-release-skip-verify
make github-release-skip-verify-overwrite
```

Пропускайте verification gate только если он уже успешно прошёл для текущего
коммита. Для редко используемых комбинаций флаги можно передать напрямую:

```sh
make github-release RELEASE_ARGS="--skip-verify --overwrite"
make package PACKAGE_ARGS="--overwrite"
```

Подробные ограничения ad hoc-подписи и порядок публикации описаны в
[инструкции по публикации](Publishing.ru.md).

## Соответствие целям и скриптам

| Цель Make | Скрипт из `Scripts/` |
|---|---|
| `build`, `build-debug`, `build-release` | `build-app-bundle.sh` |
| `verify`, `check` | `verify-mvp.sh` |
| `check-scope` | `verify-scope.sh` |
| `test-core` | `run-core-tests.sh` |
| `check-swift` | `run-swift-checks.sh` |
| `test-swift` | `run-swift-tests.sh` |
| `soak`, `soak-smoke`, `soak-accelerated` | `run-core-soak.sh` |
| `whisper-smoke` | `run-whisper-smoke.sh` |
| `package`, `package-overwrite` | `package-app-dmg.sh` |
| `github-release` и его варианты | `build-github-release.sh` |
