Ниже — полный обновлённый текст плана с применёнными правками. Файл не создаю (ask-режим), готов сохранить его в `docs/map_editor_plan.md`, когда переключитесь в Agent-режим.

---

# План реализации: редактор карт для Perimeter (Qt)

## 1. Что делаем (объём)

Отдельное десктоп-приложение-редактор карт (Qt), которое:

1. Читает/пишет мир в формате игры (`RESOURCE/Worlds/<мир>/`), рендерит воксельную поверхность.
2. Читает/пишет боевые карты (`RESOURCE/Battle/*.spg`, `RESOURCE/Worlds/*.spg`), редактирует точки спавна.
3. Умеет создавать новые миры и новые боевые карты (размер карты, генерация рельефа, изображения).
4. Даёт кисти: подъём/опускание высоты, цвет вокселя из палитры, зеропласт, две моды (Geo/Dam), формы (заполненный круг/квадрат, контур круга/квадрата, линия), кисть-изображение (прозрачные пиксели игнорируются).
5. Импорт/экспорт изображений (в т.ч. `up.tga`, `cache.tga`, `map.tga`, heightmap), импорт палитровой карты на карту.
6. Маркеры для последующей расстановки объектов/юнитов.
7. Валидация карты (в т.ч. объёмный баланс дамба относительно высоты зеропласта).

## 2. Проверенные факты, на которые опирается план

**Мир (`RESOURCE/Worlds/<мир>/`)** — набор файлов, имена заданы в `Source/Terra/VMAP.cpp:21-38`:

| Файл | Формат | Обязателен для игры |
|---|---|---|
| `output.vmp` | `sVmpHeader` ("S2T0" без сжатия / "S2T1" zlib) + 4 буфера `VxG,VxD,Atr,Sur` по `XS_Buf*YS_Buf` | да (`fullLoad`) |
| `world.ini` | INI: `[Global Parameters] Map Power X/Y` → `H_SIZE=1<<power` (лимит `MAX_H_SIZE_POWER=14`, `Source/Terra/terra.h:13`), `GeoNet Power`, `Section Size Power`, `Minimal Square Power`, `hZeroPlast`, `Version=1.4`, `[Storage] File Name=output` | да (`analyzeINI`) |
| `inGeo.act`, `inDam.act` | 768 байт BGR (256×3) | да (`loadGeoDamPal`) |
| `geoPal.xml`, `geoTx.xml` | XML (палитра 256 цветов и 3D-текстура геослоя) | да при `_TX3D_LIBRARY_` (сборка игры такая) |
| `leveledSurfaceTexture.tga` | палитровый TGA, размер — степень 2 и ≤ размера карты | да, иначе `xassert` (`VMAP.cpp:2464-2490`) |
| `cache.tga` | 24-бит несжатый TGA `H_SIZE×V_SIZE` | да, иначе `xassert` (`VMAP.cpp:935-941`) |
| `map.tga` | TGA (миникарта/радар, размер произвольный) | желателен (UI) |
| `up.tga` | TGA; кодом игры не читается (см. `docs/map_logic.md:276`), в проекте файл есть во всех мирах | для игры не нужен |
| `hardness.bin` | `uint16 w, uint16 h` + упакованные 2-битные значения | опционален (`loadHardness`) |
| `output.vsc`, `output.vpr` | процедурная генерация (используются только если нет `output.vmp`) | нет |

**Буферы карты:** `XS_Buf = H_SIZE`, `YS_Buf = V_SIZE` (`VMAP.cpp:567`), т.е. по 1 байту на клетку карты. Высота = `(VxG<<5)|(Atr&0x1F)` (1/32 м), `SGetAlt` (`VMAP.h:823`), дамба приоритетнее гео; `PutAltGeo/PutAltDam` (`VMAP.h:838-869`) поддерживают инвариант «дамб не ниже гео».

**Атрибуты:** `AtrBuf`: `At_LEVELED=0x80`, `At_ZEROPLAST=0xC0`, `At_SOOT=0x40`, `At_SHADOW=0x20`; `GABuf`: `GRIDAT_MASK_HARDNESS=0x3`, `GRIDAT_TALLER_HZEROPLAST=0x4`, `GRIDAT_LEVELED=0x8`, `GRIDAT_ZEROPLAST=0x10`, `GRIDAT_BUILDING=0x20`, `GRIDAT_MASK_CLUSTERID=0xFF00` (`VMAP.h:91-106`).

**Готовые API Terra, которые редактор переиспользует без правок:**
- загрузка мира: `prepare(...)` + `selectUsedWorld(int)`/`wTable` (вариант игры, `VMAP.cpp:484-548`), `fullLoad`, `loadLeveledTexture`, `initGrid`, `loadHardness`;
- сохранение: `save3Buf`/`save3BufOnly` (S2T0), `saveGeoDamPal`, `f3d.saveVariable`, `saveHardness`;
- рендер: `RenderPrepare1` (`LAND.cpp:1045`), `regRender(x1,y1,x2,y2)` (`LAND.cpp:892`), `RenderStr`, `getColor`/`getColor32` (`VMAP.h:615-688`), таблицы `Sur2Col`, `Sur2Col32`, `Tex2Col*`, `SurZP2Col*`, `LvdTex` (текстура выровненных зон);
- вид: `viewV2D(...)` — программный наклонный 3D-вид воксельной поверхности (`VMAP.cpp:2041`, параметры камеры — члены `vrtMap`: `turnAngleAroundZ/focusC/cameraDZ/cameraAngleLean`, инициализация `VMAP.cpp:205-218`); `scaling16(...)` — 2D-вид сверху с масштабом;
- кисти: `tools.h` — `individualToolzer<_T2TE_...>` (`tVoxSet/tVoxSetAll`, `influenceDZ`, `influenceBM`), эффекты `T2TE_CHANGING_TERRAIN_HEIGHT`, `T2TE_ALIGNMENT_TERRAIN_4ZP`, `T2TE_PLOTTING_*`, `T2TE_ERASE_ZP` и т.д. (`tools.h:1059-1500`), готовые тулзы `toolzerChangeTerHeight`, `toolzerAligmentTerrain4ZP`, `toolzerEraseZP` и др.;
- Undo/Redo: `UndoDispatcher_*` (`undoDispatcher.cpp`, `VMAP.h`);
- формы масок: `DrawBWCyrcle`/`DrawBWQuadrate` (`tools.cpp:212,237`), радиусные таблицы `xRad/yRad/maxRad`.

**Важное ограничение:** включать `_SURMAP_` для Terra нельзя — код под этим макросом частично мёртв/отсутствует: `terra.h:6` включает `_xtool.h` (нет в репо), `tools.cpp:278` — `../Surmap5/utl/3d_shape.h` (нет в репо), `S3Danalyze` ссылается на легаси-глобалы. Поэтому все «редакторные» функции (`wrldShot`, `putTgaComplete2AllMap`, `saveAllWorldAs`, фильтры `FilterGeoTerrain/FilterDamTerrain`) редактор реализует сам поверх публичных членов `vrtMap` (структура `vrtMap` — публичная).

**Сборка:** `Terra` уже собирается как static-lib с `-D_PERIMETER_ -D_TX3D_LIBRARY_` (root `CMakeLists.txt:373-374`); редактор может просто линковать `Terra`, `tx3d`, `XTool` (+ `ZLIB::ZLIB`; `Source/XTool/CMakeLists.txt:16`). Нужны шимы для символов, живущих в игре/Util: `ReadIniString`/`WriteIniString`/`IniManager` (`Util/SystemUtil.cpp`), `logicRND*` (`Util/DebugUtil.cpp`), `terEnergyBorderCheck` (`Game/Player.cpp:1213`), `currentShortVersion` (`Game/Runtime.cpp:66`). Заголовок `Util/SystemUtil.h` тянет SDL2 — включаем SDL2 только как заголовки/линк.

**Боевые карты:** `.spg` — текст XPrm (`MissionDescriptionPrm` + `SavePrm`), см. `build/Source/Debug/RESOURCE/Battle/COMPASS.spg`; у каждого игрока `players[i].frame = "struct SaveUnitFrameData" { unitID, attributeID=UNIT_ATTRIBUTE_FRAME, position{x,y,z}, orientaion{s,x,y,z}, …, squad = "struct SaveUnitSquadData" { … squadMembers, squadToFollow … } }`; в `manualData`: `zeroLayerHeight`, `cameras` (`Camera0..N`, путь из точек `position{x,y}`, `psi`, `theta`, `distance`), `controls` (136+ записей), `players[].TriggerChainNames`. Определения структур — `Source/Util/Save.h` (`SaveUnitData:217`, `SaveUnitRealData:257`, `SaveUnitSquadData:361`, `SaveUnitFrameData:434`, `SavePlayerData:2591`, `SaveManualData:2670`, `SavePrm:2748`), сериализация `MissionDescription` — `Source/Network/NetPlayer.h:180-215`. Игра грузит `.spg` в `terUniverse::universalLoad` (`Game/Universe.cpp:793-994`), игроки восстанавливаются из `SavePrm.players[i].frame`; `.gmp`/`.dat` — опциональны (`Universe.cpp:864-874`), но редактор их игнорирует (см. T8.5).

**XPrm-архив** (`Util/XPrmArchive.h/.cpp`) — потоковый парсер; чтение пропускает неизвестные узлы, запись пишет только то, что есть в структурах. Enum-поля пишутся как идентификаторы (`UNIT_ATTRIBUTE_FRAME`) через `EnumDescriptor` (`Serialization.h:194`, регистрация `add()` — `protected`), неизвестный идентификатор при чтении молча превращается в 0 → для round-trip нужен свой реестр идентификаторов.

**Ошибки:** `ErrH.Abort` — `[[noreturn]]` (`XTool/xerrhand.h:51`), значит редактору нужен preflight-валидатор и аварийный автосейв, а не «поймать исключение».

## 3. Ключевые архитектурные решения

1. **Отдельное приложение `map_editor` (Qt6 Widgets), не встроенное в игру.** Источники — `Source/MapEditor/`, опция CMake `OPTION_MAP_EDITOR` (по умолчанию OFF), `ADD_SUBDIRECTORY("MapEditor")` в `Source/CMakeLists.txt` под условием. Игровая сборка не меняется.
2. **Переиспользуем `Terra` как есть** (без `_SURMAP_`), все редакторные операции — поверх публичного API и публичных буферов. Изменения в существующем коде: **нулевые или минимальные** (в идеале — ни одной правки вне `Source/MapEditor/`).
3. **Три слоя:**
   - `MapEditorCore` — без Qt: сессия/мир, кисти, файлы, `.spg`, изображения, валидация;
   - `MapEditorSupport` — шимы (`logicRND`, `ReadIniString/WriteIniString/IniManager`, `terEnergyBorderCheck`, `currentShortVersion`, инициализация content-root, обработчик `ErrH`);
   - Qt UI (`MapEditorApp`) — окна/панели/вьюпорт; вся работа с `vMap` — в UI-потоке (Terra не потокобезопасна), длинные операции — с прогресс-диалогом.
4. **Вьюпорт:** три режима — 2D-сверху 1:1 (собственный рендер через `getColor32`, кэш в `QImage` + обновление только грязных прямоугольников через `regRender`/`changedT`), 2D с масштабом (`scaling16` или свой билинейный), наклонный 3D (`viewV2D`, поворот/наклон/зум). Оверлеи (кисть, маркеры, сетка, спавны, маски) — рисуются Qt поверх.
5. **Инструменты:** используем `individualToolzer` из `tools.h` (готовые эффекты + сглаживание + вероятностная кромка), формы масок генерируем сами (заполненный/контурный круг/квадрат — через радиусные таблицы `xRad/yRad` или формулу; линия — Брезенхэм + штамп формы), кисть-изображение — маска из TGA/PNG (альфа=0 → 0) + масштаб/поворот + масштабирование силы.
6. **Undo/Redo:** через `UndoDispatcher_PutPreChangedArea`/`Undo`/`Redo` Terra (тот же механизм, что в игре) — кнопки + Ctrl+Z/Y.
7. **Совместимость файлов:** редактор пишет ровно те файлы, что ждёт игра (см. §2), `output.vmp` — в формате S2T0 (как SurMap3); сжатие S2T1 — опцией (zlib есть).
8. **`.spg`:** `XPrmArchive` + зеркальные структуры в `Source/MapEditor/core/spg/` (глобального скоупа, имена 1:1 как в игре — иначе `typeid`-имя в файле не совпадёт), плюс динамический `EnumRegistry` (идентификаторы из базовой таблицы + из самого файла), что даёт полный round-trip. Новые карты — из шаблона `data/battle_template.spg` (по образцу `RESOURCE/Battle/COMPASS.spg`), у которого правим `worldName`, количество игроков, позиции фреймов/сквадов/камер, `zeroLayerHeight`, `playersData`.
9. **Маркеры — сайдкар-файл рядом с миром** (`<мир>/<имя>.medit.json`). В `.spg` не экспортируются и никак не покидают сайдкар.
10. **Ошибки:** preflight-проверки до вызовов Terra + `ErrH.SetCrash()` → автосейв проекта и понятное сообщение при следующем запуске.
11. **Моды не поддерживаются:** папки `mods/*` как источники миров и карт не сканируются — редактор работает только с папкой игры.

## 4. Структура каталогов и цели сборки

```
Source/MapEditor/
  CMakeLists.txt
  shim/            StdAfx.h (минимальный, для XPrmArchive.cpp), shims.cpp/.h, _xtool.h (если понадобится)
  core/            EditorSession, WorldFiles, WorldImages, BrushTool, Shapes, PaletteOps,
                   Validation, MarkerStore, spg/{SavePrmMirror.h, MissionDescriptionMirror.h, EnumRegistry.*, SpgIO.*}
  ui/              main.cpp, MainWindow, ViewportWidget, MinimapWidget, ToolPanel, PalettePanel,
                   WorldSettingsDialog, NewWorldWizard, NewBattleMapWizard, SpawnPointsPanel, ValidationDialog
  data/            battle_template.spg, world_template/ (world.ini, inGeo.act, inDam.act, geoPal.xml, geoTx.xml,
                   leveledSurfaceTexture.tga, up.tga), default_map.tga
  tests/           headless-тесты (round-trip мира, spg read/write, валидация)
```

CMake: `OPTION(OPTION_MAP_EDITOR "Build map editor" OFF)`; `find_package(Qt6 COMPONENTS Widgets)` (fallback Qt5 с предупреждением; опционально — vcpkg-фича `map-editor` с `qtbase`). Линковка: `MapEditorCore`, `Terra`, `tx3d`, `XTool`, `ZLIB::ZLIB`, SDL2 (заголовки), `Qt6::Widgets`, `${PERIMETER_COMPILE_OPTIONS}` для core.

## 5. Задачи

### T0 — Каркас сборки и шимы (блокирует всё)
- **T0.1** CMake-опция + `Source/MapEditor/CMakeLists.txt` + пустое окно Qt. *Готово, когда:* `map_editor` собирается при `-DOPTION_MAP_EDITOR=ON`, игровая сборка не затронута.
- **T0.2** `MapEditorSupport`: `logicRND`/`logicRNDi_internal`/`logicRNDf_internal`/`logicRNDfa_internal` (`RandomGenerator` из `XTool/xmath.h:165`), `ReadIniString`/`WriteIniString`/`IniManager` (simpleini, сигнатуры как в `Util/SystemUtil.h:88-117`), `terEnergyBorderCheck → 0`, `currentShortVersion = PERIMETER_VERSION`, инициализация content-root (`set_content_root_path` + `scan_resource_paths("./")` по образцу `Game/GameContent.cpp:490-514`).
- **T0.3** Preflight + `ErrH`-обвязка: проверка папки игры/мира, автологирование, `SetCrash` → автосейв; консольный тест `map_editor_tests`, который headless загружает `RESOURCE/Worlds/COMPASS`, сохраняет в temp и байт-сравнивает `output.vmp`/`world.ini`. *Готово, когда:* round-trip мира побайтово стабилен.

### T1 — Ядро: сессия и файлы мира
- **T1.1** `EditorSession`: выбор папки игры (QSettings + автодетект), список миров (`RESOURCE/Worlds/*` через `get_content_entries_directory` или свой скан), загрузка (`prepare`/`selectUsedWorld`/`fullLoad`), выгрузка, флаг «изменён».
- **T1.2** `WorldFiles`: чтение/запись `world.ini` (все секции, включая `[Rendering Parameters]`, `[Creation Parameters]`, `[Storage]`, `[GeoPal Parameters]`, `[3DF Parameters]`), `inGeo.act`/`inDam.act`, `hardness.bin`, копирование/сохранение `geoPal.xml`/`geoTx.xml`, `leveledSurfaceTexture.tga`.
- **T1.3** Сохранение мира: `save3Buf` + `saveGeoDamPal` + `f3d.saveVariable` + `saveHardness` + генерация изображений (T6) + обновление `world.ini` (размер, `hZeroPlast`, версия). *Готово, когда:* сохранённый мир открывается игрой (проверка: запуск боя/миссии на мире).
- **T1.4** Undo/Redo-обвязка, «Сохранить/Сохранить как», предупреждения.

### T2 — Вьюпорт и навигация
- **T2.1** `ViewportRenderer`: 2D-сверху 1:1 с кэшем и грязными прямоугольниками (обновление после `regRender`), учёт `At_LEVELED/At_ZEROPLAST` (через `getColor32`, т.е. с `Tex2Col32`/`SurZP2Col32`), фильтры слоя (Geo/Dam), оверлеи «зеропласт/выровнено/некопаемость» (флаги `GABuf`).
- **T2.2** `ViewportWidget`: панорама/зум (1:1…fit), курсор→координаты карты, превью кисти, сетка (`kmGrid`=4, `kmGridChA`=64), отображение маркеров/спавнов.
- **T2.3** Режим 3D-вида на `viewV2D` (поворот `turnAngleAroundZ`, наклон `cameraAngleLean`, `cameraDZ`, зум `focusC`), лимит ширины 640 (`MAX_XSIZE_VX_WINDOW`).
- **T2.4** `MinimapWidget` (`map.tga` + прямоугольник текущего вида).
- *Готово, когда:* карта 2048² открывается, панорама/зум плавные (обновление только грязной области), 3D-вид вращается.

### T3 — Кисти
- **T3.1** Каркас: `BrushParams{shape, radius, strength, mode(Geo/Dam), colorIndex, effect}`, маски форм (`filledCircle`, `filledSquare`, `circleOutline`, `squareOutline`, `line`), применение через `individualToolzer` + `UndoDispatcher_PutPreChangedArea` + `recalcArea2Grid` + `regRender`.
- **T3.2** Высота: поднять/опустить (`T2TE_CHANGING_TERRAIN_HEIGHT`), сгладить/выровнять (`T2TE_ALIGNMENT_TERRAIN_4ZP`), «выровнять относительно зеропласта» (`toolzerAligmentTerrain4ZP`, `tools.h:779-842`), режимы Geo (только `VxGBuf`) и Dam (создание/снятие `VxDBuf` с инвариантом ≥ гео).
- **T3.3** Цвет: установка `SurBuf` выбранным индексом палитры (гео — 0..127, дамба — 128..255) без изменения высоты, «пипетка».
- **T3.4** Зеропласт: поставить высоту = `hZeroPlast` (аналог `flashCircleLeveling`), пометить зеропласт (`plotCircleZL`/`GRIDAT_ZEROPLAST`), снять (`eraseCircleZL`), гарь (`T2TE_PLOTTING_SOOT`), «очень лёгкая дамба» (`T2TE_PLOTTING_VERY_LIGHT_DAM`).
- **T3.5** Кисть-изображение: загрузка TGA/PNG (Qt) → 8-битная маска (альфа=0 → 0; иначе яркость/альфа), масштаб/поворот, применение `influenceBM` или собственная реализация с масштабом дельты; превью контура.
- *Готово, когда:* все формы/режимы/кисти применяются с Undo и корректным рендером; результаты совпадают с поведением игры (проверка терраформерами в игре).

### T4 — Палитры
- **T4.1** Панель палитр: `GeoPal`/`DamPal` (из `inGeo.act`/`inDam.act`), редактирование RGB, применение (`convertPal2TableSurCol`), синхронизация `geoPal.xml`.
- **T4.2** Выбор цвета для кисти + отображение индекса/цвета под курсором.

### T5 — Параметры мира и создание нового
- **T5.1** Диалог параметров: `hZeroPlast`, размеры (только для новых карт: `createWorld(hSizePower, vSizePower)`), `GeoNet/Section/MinimalSquare`, `[Storage]` (сжатие S2T0/S2T1), `[3DF Parameters]` (`f3d.kmx/kmy/kmz/function`), цвет зеропласта (`TD/Color ZeroLayer` в `Perimeter.ini` читается в `analyzeINI`).
- **T5.2** Мастер нового мира: имя, размер (power X/Y), генерация (`buildWorld`) или плоская поверхность, копирование шаблона палитр/текстур/`up.tga`, начальные `hZeroPlast`/палитры.
- *Готово, когда:* новый мир открывается в игре и в редакторе, размеры/высоты соответствуют заданным.

### T6 — Изображения
- **T6.1** Генерация `cache.tga` (24-бит, `H_SIZE×V_SIZE`, по `getColor32`), `map.tga` (мини-карта, из `cache.tga` с масштабом). `up.tga` — пользователь импортирует изображение (PNG/JPG/BMP/TGA), редактор приводит его к нужному формату и размеру и сохраняет как `up.tga`; без импорта файл остаётся как есть.
- **T6.2** Экспорт heightmap (8-бит TGA по `SGetAlt>>5`), экспорт «вида сверху».
- **T6.3** Импорт изображения на карту: палитровый TGA → `SurBuf` (аналог `putTgaComplete2AllMap` с фильтрами по гео/дамбе/высоте/цвету) и RGB → ближайший индекс палитры.
- **T6.4** Импорт/замена `leveledSurfaceTexture.tga` с валидацией (палитровый, степень 2, ≤ размера карты).
- *Готово, когда:* после сохранения игра отображает карту и миникарту корректно.

### T7 — Маркеры
- **T7.1** `MarkerStore`: тип (юнит/здание/зона/точка), `label`, заметка, цвет, позиция; файл `<мир>.medit.json`; CRUD, список, переход к маркеру.
- **T7.2** Оверлей и панель маркеров. Экспорта в `.spg` нет — маркеры живут только в сайдкаре.
- *Готово, когда:* маркеры сохраняются между сессиями и видны на карте.

### T8 — Боевые карты (`.spg`)
- **T8.1** `SpgIO`: XPrm (собственный `shim/StdAfx.h` для `Util/XPrmArchive.cpp`), зеркальные структуры (`SavePrmMirror`, `MissionDescriptionMirror`), `EnumRegistry` (полный round-trip идентификаторов), запись `.spg` + `.sph`.
- **T8.2** Загрузка: список `RESOURCE/Battle/*.spg` (+`SCENARIO`, `SURVIVAL`, `RESOURCE/Worlds/*.spg`), резолв мира по `worldName` (`vMap.getWorldID`), загрузка мира.
- **T8.3** Точки спавна: по игроку — фрейм+сквад (и их юниты) и камера; перемещение группы с коррекцией `z` по дельте высот (`z_new = z_old + (terrain_new − terrain_old)`), чтобы сохранить исходное вертикальное смещение модели (в `COMPASS.spg` `frame.position.z=328.353` при `zeroLayerHeight=177` — смещение не выводится из рельефа); редактирование `playersData` (тип игрока/фракция/цвет/клан/сложность).
- **T8.4** Мастер новой боевой карты: мир (существующий/новый), размер, число игроков, расстановка спавнов, `zeroLayerHeight = hZeroPlast`, генерация из шаблона.
- **T8.5** Сохранение в папку игры (`RESOURCE/Battle`, `RESOURCE/Worlds`), с предупреждением о перезаписи. `.gmp`/`.dat` игнорируем полностью: не читаем, не применяем, не пишем.
- *Готово, когда:* новая карта запускается в «Сражении»: игроки появляются на заданных спавнах, камеры на месте, базы целы.

### T9 — Валидация
- **T9.1** Каркас проверок + диалог отчёта (ошибка/предупреждение/инфо, «перейти к месту»).
- **T9.2** Проверки мира: наличие/формат обязательных файлов, размеры (`H_SIZE/V_SIZE` = степень 2, ≤16384), согласованность `hZeroPlast` (0..255) между `world.ini` и `.spg`, размеры палитр/текстур, `hardness.bin`.
- **T9.3** Проверка объёмного баланса: по всей карте суммируется объём превышений поверхности над плоскостью зеропласта и объём впадин под ней (объём = отклонение × площадь клетки). Карта считается сбалансированной, если дисбаланс объёмов в пределах настраиваемого допуска (абсолютного или в % от объёма впадин). Иначе — предупреждение, что выравнивание до зеропласта потребует импорта/экспорта грунта.
- **T9.4** Проверки боевых карт: существование мира, соответствие числа игроков, спавны внутри карты и на проходимой поверхности, уникальность `label`, наличие камер.
- **T9.5** Автофиксы: пересоздать `cache.tga`/`map.tga`, выставить `zeroLayerHeight`, нормализовать `playersShufflingIndices`.

### T10 — Документация, упаковка, приёмка
- **T10.1** `docs/map_editor.md` (сборка, сценарии, форматы, ограничения) + дополнение `docs/map_logic.md`; обновление `AGENTS.md`/`MODDING.eng.md` при необходимости.
- **T10.2** CMake/vcpkg-интеграция (Qt), install-правила, `.gitignore` для temp; поддержка сборки и работы на Windows и Linux (macOS не поддерживается).
- **T10.3** Приёмочный прогон: миры (загрузка/правка/сохранение), боевые карты (новая/правка), импорт/экспорт изображений, валидация; чек-лист проверок в самой игре.

## 6. Порядок и параллелизация

```
T0 ──┬─► T1 ──┬─► T2 ──┬─► T3 ──► T4 ──► T9 ──► T10
     │        │        ├─► T7
     │        ├─► T5   │
     │        ├─► T6 ──┘
     └────────┴─► T8 ─────────────► T9
```
- T0 — критический путь, делать первым и одним исполнителем.
- T2/T3/T5/T6/T7/T8 могут идти параллельно после T1 (T8 зависит только от T0/T1; T3 — от интерфейса `ViewportRenderer`).
- T9/T10 — в конце; T9 частично параллельно с T3–T8 (по интерфейсу проверок).

## 7. Тестирование
1. **Headless-тесты** (`map_editor_tests`): round-trip мира (байт-в-байт `output.vmp`, семантически `world.ini`), round-trip `.spg` (чтение→запись→чтение, сверка всех полей), валидатор на эталонных картах, генерация изображений (размеры/формат).
2. **Проверки в игре** (ручные, чек-лист): мир грузится, рельеф/цвета/зеропласт/дамбы совпадают с редактором; новая боевая карта стартует; правка существующей карты не ломает базы/камеры/скрипты.
3. **Краш-тесты**: открытие битых/чужих файлов → сообщение, без падения (preflight).

## 8. Риски и меры

| Риск | Мера |
|---|---|
| `ErrH.Abort`/`xassert` завершают процесс | preflight-проверки, `SetCrash` + автосейв, релизная сборка без ассертов (`NDEBUG`) |
| Производительность программного рендера 2048² | грязные прямоугольники, кэш `QImage`, режим «fit»/тайлы для больших карт |
| Точность round-trip `.spg` (enum-идентификаторы, `ShareHandle`-типы) | `EnumRegistry` из файла + зеркальные структуры с точными именами/порядком полей; автотест «чтение→запись→diff» |
| `z`-смещения моделей в `.spg` | правило «дельта высот», шаблонные блоки, проверка в игре |
| Несовместимость версий (`.spg` `version`, `_FINAL_VERSION_`) | писать `currentShortVersion`, собирать с теми же define, что игра |
| Qt в сборке проекта | опция OFF по умолчанию, Qt только для таргета редактора |
| Неизвестные/легаси-миры (S2T1, отсутствие `leveledSurfaceTexture.tga`) | авто-конвертация/генерация недостающих файлов, отчёт валидации |

---

Все правки применены: `up.tga` (T6.1), маркеры-сайдкар (§3 п.9, T7.2), объёмная формула баланса (T9.3), игнор `.gmp`/`.dat` (T8.2, T8.5, пометка в §2), Windows+Linux (T10.2), без модов (§3 п.11), раздел открытых вопросов удалён. Переключитесь в Agent-режим — сохраню этот текст в `docs/map_editor_plan.md` и начну с T0.