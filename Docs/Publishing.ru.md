# Публикация LocalScribe на GitHub

Репозиторий настроен на ветку `main`. Временные результаты сборки, локальные
модели, записи, базы данных и системные файлы macOS исключены через
`.gitignore`.

## Перед первой публикацией

1. Запустите проверки:

   ```sh
   Scripts/verify-mvp.sh
   ```

2. Просмотрите состав первого коммита:

   ```sh
   git status
   git diff --cached --stat
   ```

3. Проверьте лицензионные файлы `LICENSE.md`, `NOTICE` и
   `THIRD_PARTY_NOTICES.md`. Оригинальный код LocalScribe опубликован по
   PolyForm Noncommercial 1.0.0: личное и другое некоммерческое использование
   разрешено, а коммерческое использование и продажа требуют отдельного
   разрешения правообладателей. Не называйте проект open source: это
   source-available лицензия, не одобренная OSI.

4. Проверьте тексты и метаданные: название, описание, версию в
   `Config/Info.plist`, контакт для обратной связи и ссылки в README.

## Создайте репозиторий на GitHub

На GitHub выберите **New repository**, задайте имя `LocalScribe` и не добавляйте
README, `.gitignore` или лицензию автоматически: локальный проект уже содержит
README и `.gitignore`.

После создания GitHub покажет URL. Выполните из корня LocalScribe:

```sh
git commit -m "Initial public release"
git remote add origin https://github.com/OWNER/LocalScribe.git
git push -u origin main
```

Замените `OWNER` на имя пользователя или организации GitHub. Если для HTTPS
настроена не авторизация, используйте SSH-адрес, показанный GitHub:

```sh
git remote add origin git@github.com:OWNER/LocalScribe.git
git push -u origin main
```

Добавление remote и отправка кода требуют созданного репозитория и выбранного
владельца, поэтому эти действия намеренно выполняются после создания страницы
на GitHub.

## Что не должно попасть в коммит

- каталоги `.build/`, `Build/`, `DerivedData/` и готовые `.app`;
- модели `*.bin` и `*.gguf`;
- аудиозаписи `*.wav`, `*.m4a`, `*.mp3`, `*.mp4`;
- локальные базы `*.db`, файлы `*.db-wal` и `*.db-shm`;
- `.DS_Store` и пользовательские настройки Xcode.

Перед каждым push полезно проверять `git status` и размер новых файлов. GitHub
не принимает обычные Git-объекты размером более 100 MiB; модели распознавания
следует распространять отдельно, а не через этот репозиторий.

## Технический GitHub pre-release

Для небольшой группы технических тестировщиков можно создать ad hoc-подписанный
DMG одной командой:

```sh
Scripts/build-github-release.sh
```

Скрипт запускает `verify-mvp.sh`, после него обязательно пересобирает
оптимизированный release bundle, создаёт DMG в `dist/`, проверяет образ и
записывает рядом SHA-256. Имя артефакта формируется из версии в
`Config/Info.plist` и фактической архитектуры приложения, например:

```text
dist/LocalScribe-0.1.0-macOS-arm64.dmg
dist/LocalScribe-0.1.0-macOS-arm64.dmg.sha256
```

Существующий артефакт той же версии по умолчанию не перезаписывается. Для
повторной внутренней сборки используйте:

```sh
Scripts/build-github-release.sh --overwrite
```

Если полный набор проверок уже был успешно выполнен для текущего коммита,
его можно явно пропустить:

```sh
Scripts/build-github-release.sh --skip-verify --overwrite
```

Отдельный упаковщик не пересобирает приложение и полезен после ручной release
сборки:

```sh
Scripts/build-app-bundle.sh release
Scripts/package-app-dmg.sh
```

Загрузите оба файла из `dist/` в GitHub Release и отметьте релиз как
**Pre-release**. Тестировщик должен перенести LocalScribe в `/Applications`,
один раз попытаться открыть его, затем выбрать **System Settings → Privacy &
Security → Open Anyway**. Модель Whisper в DMG не входит. После обновления ad
hoc-подпись изменится, поэтому macOS может снова запросить Microphone и Screen
Recording.

## Публичный релиз приложения

Скрипт `Scripts/build-app-bundle.sh` создаёт ad hoc-подписанную локальную сборку.
Не публикуйте её как готовый установщик для широкой аудитории: для нормального
запуска на чужих Mac нужны Developer ID-подпись, hardened runtime, нотариальная
проверка Apple и проверенный сценарий выдачи разрешений. До появления такого
процесса безопаснее описывать проект как сборку из исходного кода.
