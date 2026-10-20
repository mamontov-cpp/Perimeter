# Архитектура редактора карт

Документ фиксирует принятые архитектурные решения по редактору карт (см.
`docs/map_editor_plan.md`). Обновляется по мере выполнения задач.

## 1. Границы и слои

Редактор — отдельное приложение `map_editor` (Qt6 Widgets), собирается только
при `OPTION_MAP_EDITOR=ON`. Игровая сборка не зависит от Qt. Исходники —
`Source/MapEditor/`, три слоя:

| Слой | Зависит от Qt | Ответственность |
|---|---|---|
| `MapEditorCore` | нет | сессия/мир, файлы (`WorldFiles`), INI (`IniDocument`), preflight-проверки, изображения, кисти, `.spg`, валидация |
| `MapEditorSupport` | нет | шимы для живущих в игре символов: `init_content_root`, `ErrH`, `logicRND`, `IniManager`, `terEnergyBorderCheck`, `currentShortVersion` |
| `MapEditorApp` (`ui/`) | да | окна/панели/вьюпорт; вся работа с `vMap` — в UI-потоке |

`MapEditorCore` линкуется с `Terra`, `tx3d`, `XTool`, `Util`, `SDL2::SDL2`
(заголовки), `ZLIB::ZLIB`. Terra переиспользуется как есть, без `_SURMAP_`.

## 2. `EditorSession` — жизненный цикл мира

`EditorSession` (в `core/`, без Qt) инкапсулирует глобальный `vMap` Terra.

**Установка папки игры.** `setGameDir()` проверяет, что путь существует и
содержит `RESOURCE/Worlds/`. `init_content_root()` (в `MapEditorSupport`)
выставляет content-root = текущий рабочий каталог и сканирует ресурсы
(`set_content_root_path` + `scan_resource_paths("./")`).

**Скан миров.** `scanWorlds()` перечисляет каталоги `RESOURCE/Worlds` через
`get_content_entries_directory` — та же логика, что в `vrtMap::prepare`.

**Загрузка.** `loadWorld(index)`:
1. `fs::current_path(gameDir_)` + `init_content_root()`;
2. `refreshWorldTable()` — наполняет `vMap.wTable`;
3. резолв мира по имени (`vMap.getWorldID`);
4. `vMap.selectUsedWorld(terraIdx)` (analyzeINI + worldPrepare + данные) +
   `vMap.fullLoad()`.

**Выгрузка.** `unloadWorld()` освобождает именно буферы мира:

```cpp
vMap.delLeveledTexture();  // обязательно до освобождения VxDBuf
vMap.releaseMem4Buf();     // VxG/VxD/Sur/Atr/Rnr/Sup/GV/GA
vMap.worldFree();          // сеточные карты
```

`vMap.worldFree()` сам по себе освобождает только сеточные карты, поэтому
`isWorldLoaded()` (проверяющий `VxGBuf`) остался бы `true`. Полная
последовательность скопирована из `vrtMap::~vrtMap` (`Source/Terra/VMAP.cpp`),
но **без** `landRelease()` — радиусные таблицы `prepare()` одноразовые на
процесс (см. `docs/context.md`).

**Флаг изменений.** `isModified()` = `vMap.worldChanged || modified_`;
`setModified()` синхронизирует оба.

## 3. `WorldFiles` — доступ к папке мира

`WorldFiles(worldDir)` — типизированный доступ к файлам одного мира по именам
из `Source/Terra/VMAP.cpp:21-38`: `world.ini` (все секции через `IniDocument`),
`inGeo.act`/`inDam.act`, `hardness.bin`, `geoPal.xml`/`geoTx.xml`,
`leveledSurfaceTexture.tga`, `cache.tga`/`map.tga`. Неизвестные ключи INI при
записи сохраняются; чужие файлы не трогаются (см. `docs/context.md`).

## 4. Сохранение мира (T1.3)

`saveWorld()` (в `EditorSession`) выполняет конвейер:

1. **Буферы/палитры/[3DF].** `vMap.save3Buf()` — внутри вызывает
   `save3BufOnly()` (S2T0, `output.vmp`), `saveGeoDamPal()`
   (`inGeo.act`/`inDam.act`) и `f3d.saveVariable()` (`[3DF Parameters]`).
2. **`hardness.bin`.** `vMap.saveHardness()` — только если файл уже есть
   («Сохранить» не добавляет файлы, которых автор не просил).
3. **`world.ini`.** `writeMapPower(H_SIZE_POWER, V_SIZE_POWER)`,
   `writeHZeroPlast(hZeroPlast)`, `writeStorageVersion` (сохраняет текущую
   версию, иначе `1.4`).
4. **Изображения** (генерация T6):
   - `cache.tga` — 24-бит несжатый TGA `H_SIZE×V_SIZE`, пиксели из
     `vMap.getColor32(offsetBuf(x,y))` (BGR). Игра `xassert`-ит на этом файле
     при `fullLoad`, поэтому он перегенерируется на каждом сохранении;
   - `map.tga` — миникарта: та же картинка, масштабированная не более чем до
     256×256 (ближайший сосед).

После успеха сбрасываются `modified_` и `vMap.worldChanged`.

Общий рендер-хелпер `renderMapToBgr()` используется обоими генераторами, чтобы
`cache.tga` и `map.tga` всегда показывали одну и ту же картинку.

## 5. Тесты

Headless-тесты `map_editor_tests` (`tests/`), по файлу на модуль, порядок
регистрации:

| Порядок | Файл | Что проверяет |
|---|---|---|
| 10 | `PreflightTests` | preflight-валидация папки игры |
| 20 | `WorldRoundTripTests` | байт-в-байт `output.vmp`, семантика `world.ini`; готовит общий `TestContext` |
| 30 | `WorldFilesIniTests` | чтение/запись `world.ini` |
| 40 | `WorldFilesDataTests` | палитры/`hardness.bin`/XML/текстура |
| 50 | `SaveWorldTests` | конвейер `saveWorld()` + перезагрузка |

Тесты пишут только во временный content-root (`%TEMP%/map_editor_roundtrip`),
исходный мир не изменяется. `SaveWorldTests` строит изолированный content-root
(копия сохранённого мира + общие ресурсы `RESOURCE/GeoTx` + `Perimeter.ini`),
чтобы `fullLoad()` прошёл так же, как в игре.
