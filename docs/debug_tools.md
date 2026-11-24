# Отладочные инструменты игры и рецепты диагностики

Сводка встроенных средств отладки `perimeter.exe` и рабочих приёмов, которые
использовались для диагностики крашей/багов этой ветки (см. `context.md` —
разделы про крахи редактора, Шокера и undo O(N²)).

## Интерактивной текстовой консоли НЕТ

Консоли в стиле Quake (`~`) в игре нет. Ближайшие аналоги — debug key
handler и in-game редакторы параметров (ниже).

## Запуск с аргументами (синтаксис!)

`check_command_line()` требует формата `ключ=значение` — голый флаг
(`lightingtest`) НЕ сработает, нужно `lightingtest=1` (XUTIL.cpp:
поиск по префиксу `switch_key + "="`). Это дважды подставляло ловушку
при диагностике — «флаг стоит, а код не отрабатывает».

Аргументы с `=` и пробелами внутри — в кавычках. Путь к миру/миссии
тоже аргумент: `open=battle/extdemo`, `mission=10xmars`.

Важно: `open=`/`mission=`/`save=` обрабатываются в блоке
`if(!MainMenuEnable)` (GameShell.cpp) — без `mainmenu=0` игра просто
висит в главном меню, и миссия НЕ грузится (в логе нет блока
`==== MissionDescription ====` — первый признак).

Рабочая строка запуска прямо в бой:
```
perimeter.exe "mainmenu=0" "open=battle/extdemo" "start_splash=0" \
              "disable_sound=1" "console=1" "debug_key_handler=1"
```

## console=1 — это НЕ консоль

Перенаправляет лог из файла (`%APPDATA%\Roaming\K-D LAB\Perimeter\logfile.txt`)
в stdout/stderr запускающей консоли. Для диагностики обязателен: без него
`fprintf(stderr,...)` уходит в logfile. В логе видно факт перенаправления:
строка `Using console stdio output` (ок) против `Redirecting console stdio
output into log file...` (аргумент не доехал).

## Debug key handler — главный внутриигровой инструмент

Включение: **Ctrl+Shift+F1**.
- Debug-сборка (`CMAKE_BUILD_TYPE=Debug` → `PERIMETER_DEBUG`, у мульти-конфиг
  генератора — конфиг Debug) — работает всегда.
- Release — только с `debug_key_handler=1` на командной строке
  (иначе комбинация проигнорирована).

После включения **F1 в игре показывает экранную подсказку по всем клавишам**
(текст `<keys_help>` из локализации). Основные:

| Клавиша | Действие |
|---|---|
| F1 | подсказка по клавишам (переключение) |
| F2 | debug watch / writers состояния тел (Shift — режим записи) |
| F3 | show mode |
| **F4** | **редактор параметров** (`editParameters()`, см. ниже) |
| F5 | отладочная информация vMap (перерисовка карты регионов) |
| F6 | profiler (Shift — StartCaptureFrame на Debug) |
| F9 / Shift+F9 / Ctrl+F9 / Ctrl+F10 / Ctrl+F11 | пути камеры: точка, удалить, проиграть/стоп, сохранить, запись ролика |
| F12 / Shift+F12 / Ctrl+Shift+F12 | принудительная победа / поражение / (игровое сообщение) |
| **Ctrl+Shift+C / Ctrl+Shift+V** | **мгновенный сейв/лоад миссии** — бесценно для repro багов (снимок состояния вселенной и загрузка его обратно) |
| Ctrl+R | render debug UI (см. ниже) |
| Ctrl+F4 | mission editor в игре (аналог запуска с `edit=1`) |
| N | debug_variation; M — ограничение камеры; `[`/`]` — LOD карты |
| Ctrl+Enter (Debug) | TriggerEditor — редактор триггерной цепи (Win32 UI, в коде помечен TODO перенести в dev UI) |

## Редактор параметров F4 (`GameShell::editParameters()`)

Popup-меню из 9 разделов; каждый открывает generic property-tree редактор
`EditArchive` (Source/Util/EditArchive.h) — дерево строится по reflection-
метаданным `SERIALIZE` (русские/английские подписи полей). Редактирует
«сырые» поля структур без валидации.

| Пункт | Что редактирует | Источник данных |
|---|---|---|
| Заголовок миссии | `MissionDescription` (имя, мир, игроки) → в файл миссии | `CurrentMission` |
| Миссия | `manualData` (цвета/кланы игроков, саундтреки, сплайны камер, флаги интерфейса) → в файл миссии | `savePrm().manualData` |
| Глобальные параметры | `GlobalAttributes`: debug-масштабы, таблицы рас, стоимость блоков, параметры поля, **уровни сложности**, звуковые события, **baseUnitsMax (250)**, возврат энергии при продаже, параметры ковшей | `globalAttr` (Config.prm) |
| **Атрибуты** | **вся база атрибутов юнитов**: урон, weaponSetup, элементы damageMolecula, радиусы, анимации, звуки — то же скомпилированное содержимое `UnitAttribute.prm` | `attributeLibrary` |
| Звуки | все определения звуков (имена, файлы, группы) | `soundScriptTable` |
| Интерфейс | параметры скриптов интерфейса | `interfaceAttr` |
| Физические параметры | rigid-body параметры юнитов/снарядов | `rigidBodyPrmLibrary` (RigidBody.prm) |
| Миссия все данные | весь `SavePrm` одним деревом | `savePrm()` |
| Debug.prm | отладочный архив | `debugPrm_` |

Семантика правок: правки миссии (первые два пункта + «все данные»)
пишутся в файл миссии; правки атрибутов/глобалов — на текущую сессию.
Изменение высоты зеропласта дополнительно пишется в `world.ini`
(`hZeroPlast`).

## Render debug UI (sokol-бэкенд, Dear ImGui)

- В игре: **Ctrl+R** (через DebugKeyPressed).
- Или сразу при старте: `render_debug=1`; `render_debug=b/i/s/r` открывает
  конкретное окно (buffers / images / samplers / render states).
  Реализация: `cSokolRender::DebugUISetEnable` (SokolRenderState.cpp).
  У D3D9-бэкенда этого UI нет (stub).

## Прочие аргументы запуска из `--help` (show_help в Runtime.cpp)

- `edit=1` — mission editor mode (диалог выбора миссии при запуске).
- `explore=1` — открывает редактор Debug.prm и закрывает игру.
- `testcrash=0/1/2` — искусственные краши для проверки репортинга.
- `stack_frames`/`stack_reference` — параметры из WER-дампов для
  восстановления стека тем же бинарником.
- `mods=0` — отключить загрузку модов (`build/Source/Debug/Mods`).
- `content_debug=1`, `content_dump_debug=1`, `not_triggerchains_binary=1`,
  `xprm_compiler`, `show_fps=0/1`, `convert=1` (сохранить карту и выйти).

## Диагностика крашей: рабочий рецепт (использовался в этой ветке)

1. **Дампы WER**: `%LOCALAPPDATA%\CrashDumps/*.dmp`. Для `.exr`/стека —
   cdb (Windows Kits): локальные символы БЕЗ `srv*`, иначе зависает на
   сети:
   ```
   cdb.exe -z dump.dmp -cf script.txt
   :: script.txt:
   :: .sympath D:\...\build\Source\MapEditor\Release;D:\...\build\Source\Release
   :: .exr -1
   :: kn 30
   :: q
   ```
   Без PDB (Release без символов) смотреть нечего — строить
   RelWithDebInfo или смотреть Debug.
2. **Живой процесс под cdb**: `cdb.exe -g -cf script.exe ... args` —
   `-g` пропускает initial breakpoint; на первом же исключении
   выполняются команды из `-cf` (`~0s` — переключиться с потока
   break-in, `kn N`, `dv /V` — локалы, `qd` — отсоединиться).
   Debug CRT ловит двойное удаление/порчу кучи сразу (0xfeeefeee в
   адресах = уже освобождённая память).
3. **Сэмплирование зависания**: повторные attach'и `cdb -p PID` с `kn` —
   poor man's profiler (так был пойман O(N²) в finalizePendingUndoGroup).
4. **gflags/pageheap**: `gflags /p /enable perimeter.exe /full` ловит
   порчу кучи по первому же выходу за границу, но требует elevated.
5. **Связка с autotest-режимом**: MAP_EDITOR_AUTOTEST у редактора;
   у игры — `mainmenu=0 open=battle/<spg>` + таймаут: миссия сама
   выходит через ~2.5 мин, деструктор вселенной отрабатывает в конце —
   краши «на выходе из скирмиша» ловятся и headless.

## Пайплайн игровых скриптов (когда правишь .prm/.inl)

- Источники: `Source/Scripts/*.prm` + `*.inl` (баланс — GameBalance.inl,
  звуки — Sound.inl). Генератор — консольный `XPrm` (таргет `XPrm`,
  `Source/XPrm/XPrm.cpp`), обвязка — таргет `ScriptsCompile`
  (`Source/Scripts/CMakeLists.txt`): `XPrm Source/Scripts/X.prm -SoSource`
  из каталога `Source/`.
- По умолчанию `OPTION_PROCESS_SCRIPTS=OFF` — .cppi/.hi лежат в репо
  как есть и НЕ перегенерируются при сборке; после правки источников
  нужен ручной `cmake --build build --target ScriptsCompile`.
- Генератор переписывает ВСЕ выходные файлы: нерелевантные изменения
  (переформатирование встроенных блоков кода, концы строк) откатывать
  перед коммитом; бинарно-точный откат: `git cat-file blob HEAD:<f> | cat > <f>`
  (обычный `git checkout`/`>` переводят концы строк).
- После перегенерации .cppi — полная пересборка игры (~10 мин Debug).
- Атрибуты/баланс/звуки юнитов можно сравнить с каноном PerimeterET:
  `build/Source/Debug/Mods/PerimeterET/Scripts/AttributeLibraryExtra`.
