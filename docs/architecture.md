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
   `vMap.fullLoad()`;
5. `vMap.WorldRender()` — инициализация освещения `RnrBuf`/`At_SHADOW`, ровно
   то, что после загрузки делает `terUniverse::universalLoad`
   (`Source/Game/Universe.cpp`). Без этого `fullLoad()` оставляет `RnrBuf`
   неинициализированным (его цикл `RenderStr` закомментирован в
   `VMAP.cpp:924`), и любой `getColor32()` — вьюпорт, `cache.tga` — читал бы
   мусор (см. `docs/context.md`).

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
     `ViewportRenderer` (Composite-фильтр, без оверлеев, т.е. ровно
     `vMap.getColor32()`, BGR). Игра `xassert`-ит на этом файле при
     `fullLoad`, поэтому он перегенерируется на каждом сохранении;
   - `map.tga` — миникарта: та же картинка, масштабированная не более чем до
     256×256 (ближайший сосед).

После успеха сбрасываются `modified_` и `vMap.worldChanged`; в стеке undo
запоминается позиция «последнего сохранения» — после undo/redo флаг modified
пересчитывается относительно неё (правка → сейв → undo снова «чист»).

## 4.1 Undo/Redo (T1.4)

Terra записывает undo только под `_SURMAP_` (`UndoDispatcher_PutPreChangedArea`
— заглушка в сборке игры/редактора), поэтому история хранится в сессии:
`EditorSession::recordUndoArea(x,y,w,h)` снимает «до»-состояние
`VxGBuf/VxDBuf/AtrBuf/SurBuf` над прямоугольником (клип к карте, лимит памяти
как в Terra — 4096² клеток), `undo()/redo()` обменивают снимок с буферами и
отмечают область через `regRender` (вьюпорт обновляется по обычной
dirty-конвейерке). Обязательный контракт для инструментов (T3): вызывать
`recordUndoArea()` ДО первой правки буферов. Подробности — в
`docs/context.md`.

**GUI (MainWindow).** File: Save (Ctrl+S) / Save As (Ctrl+Shift+S, имя —
диалог, папка `RESOURCE/Worlds/<имя>`, перезапись запрещена, после —
перебиндинг вьюпорта); Edit: Undo (Ctrl+Z) / Redo (Ctrl+Y) / Clear Undo
History. Доступность кнопок/действий пересчитывается во всех изменениях
состояния сессии. Диалог «несохранённые изменения» (Save/Discard/Cancel)
показывается перед загрузкой/выгрузкой мира и закрытием окна
(`closeEvent`).

Общий рендер-хелпер `renderMapToBgr()` используется обоими генераторами и
растеризует через `ViewportRenderer` (свежий экземпляр с дефолтными
настройками), чтобы `cache.tga`, `map.tga` и экранное окно не могли разойтись
ни в одном пикселе.

## 5. `ViewportRenderer` — растр 1:1-вьюпорта (T2.1)

`core/ViewportRenderer.{h,cpp}` (без Qt) — программный растеризатор вида
сверху 1:1 с кэшем и грязными прямоугольниками.

**Кэш.** `std::vector<uint32_t>`, попиксельно `0xFFRRGGBB`, `H_SIZE×V_SIZE`
построчно. Раскладка совпадает с `QImage::Format_ARGB32`, поэтому виджет (T2.2)
обернёт буфер zero-copy, а сам `MapEditorCore` остаётся без Qt и
тестируется headless — это осознанное уточнение плана §3.4 («кэш в `QImage`»).

**Конвейер `update()`** (безопасно звать каждый кадр):

1. `vMap.renderQuant()` — пересчёт освещения (`RnrBuf`/`At_SHADOW`) для областей,
   помеченных `regRender()`; очищает `vMap.renderAreas`;
2. сборка прямоугольников: полная карта после `bindWorld()`/смены
   фильтра/оверлеев, иначе боксы из `vMap.changedAreas`; список
   `changedAreas` очищается (тот же consume-и-clear-контракт, что у
   `terUniverse::quant()` в игре);
3. перерисовка боксов в кэш; возврат перерисованных `DirtyRect`
   (координаты — ячейки, `x/y/w/h`, в отличие от включительных `dx/dy` у
   `sRectS`). Пустой вектор = «ничего не изменилось».

Цвет базового слоя — `vrtMap::getColor32()` (ветки `At_LEVELED`/`At_ZEROPLAST`
через `Tex2Col32`/`SurZP2Col32`). `bindWorld()` предполагает загруженный и
освещённый мир (п.5 `loadWorld()`); вызов без него откажется.

**Фильтры слоя** (`LayerFilter`):

| Фильтр | Поведение |
|---|---|
| `Composite` | точь-в-точь `getColor32()` |
| `Geo` | цвет считается с освещением `RnrBuf & 0x7F` (гео-половина таблиц): бит 7 `RnrBuf` — признак дамбы, он переключает строки 128..255 палитры на `DamPal`, а `SurBuf` хранит общий индекс материала для обеих палитр (`PutAltDam` вообще не трогает `SurBuf`) — поэтому под дамбой гео-цвет восстанавливается точно, без аппроксимации |
| `Dam` | дамбовые ячейки — как есть, без дамбы — затемнены до 25% (`dimColor`) |

**Оверлеи** (`OverlayFlags`, поверх любого фильтра, по флагам 4×4-блока
`GABuf`): зеропласт — сильный (`GRIDAT_ZEROPLAST`, α 96) / слабый
(`GRIDAT_ZEROPLAST_IS_PRESENT`, α 40, приоритетнее «выровнено»), «выровнено»
(`GRIDAT_LEVELED`, зелёный α 72), некопаемость — красный, сила по
`GRIDAT_MASK_HARDNESS` (0 — максимум, 3 — без тинта; см. семантику в
`docs/context.md`). Палитры тинтов/альфы — публичные константы класса:
контракт, который переиспользуют тесты; `blendTint()`/`dimColor()` —
`constexpr`-стatische, тот же канал-в-канал расчёт.

Смена фильтра/оверлеев помечает всю карту; `renderMapToBgr()` (T1.3) растеризует
через этот же класс, см. §4.

## 5.5. `ViewportWidget` — Qt-виджет вьюпорта (T2.2)

`ui/ViewportWidget.{h,cpp}` — наследник `QWidget`, отображает растр
`ViewportRenderer` и добавляет навигацию/оверлеи.

**Zero-copy QImage.** `drawBaseRaster()` оборачивает `uint32_t*` кэш
`ViewportRenderer` в `QImage` с тем же layout (`Format_ARGB32`, row-major) и
рисует одним вызовом `painter.drawImage(targetRect, image, sourceRect)`. Ни одной
дополнительной копии памяти — это критично для карт 2048². При `zoom < 1.0`
включается `SmoothPixmapTransform`, при `zoom == 1.0` — `FastTransformation`
(отключён через `setRenderHint`).

**Диапазон зума «1:1…fit».** `zoom_` климпуется в `[fitScale_, 1.0]`.
`fitScale_` вычисляется в `resizeEvent` как `min(viewW/mapW, viewH/mapH)`
с отступом 12 px. `zoomToFit()` центрирует карту и ставит zoom на `fitScale_`;
двойной клик — тоже. Колесо мыши меняет zoom с шагом 1.25, центрируя на
координатах курсора.

**Панорамирование.** Средняя и правая кнопки мыши — перетаскивание.

**Сетка.** Два уровня: тонкая (шаг `kFineGridCell = 4`, соответствует `kmGrid` из
`terra.h`/`VMAP.h`) рисуется только когда расстояние между линиями ≥ 4 px;
грубая (шаг `kCoarseGridCell = 64`, соответствует `kmGridChA`) — всегда.
Линии выравниваются по началу сетки (divisible by step), а не по видимому
диапазону, чтобы избежать смещения.

**Превью кисти.** Жёлтый пунктирный контур (круг/квадрат) размером `brushRadius_ * zoom_`,
центрированный на ячейке под курсором. Видимость управляется из панели инструментов (T3).

**Координаты курсора.** `mouseMoveEvent` конвертирует экранные координаты в
карту (`screenToMap`), эмитит `cursorMapPosition(mapX, mapY)` — `MainWindow`
показывает их в статус-баре. В правом нижнем углу вьюпорта — HUD с
координатами и текущим зумом.

**Маркеры и спавны.** `drawMarkers()` / `drawSpawnPoints()` рисуют цветные
точки с подписями. Данные подаются из T7 (маркеры) и T8 (спавны) — виджет
только рендерит.

**Порядок отрисовки в `paintEvent()`:** (1) `renderer_->update()` — слить
грязные прямоугольники и пересчитать освещение; (2) фон; (3) растр;
(4) сетка; (5) превью кисти; (6) маркеры; (7) спавны; (8) HUD.

## 6. Тесты

Headless-тесты `map_editor_tests` (`tests/`), по файлу на модуль, порядок
регистрации:

| Порядок | Файл | Что проверяет |
|---|---|---|
| 10 | `PreflightTests` | preflight-валидация папки игры |
| 20 | `WorldRoundTripTests` | байт-в-байт `output.vmp`, семантика `world.ini`; готовит общий `TestContext` |
| 30 | `WorldFilesIniTests` | чтение/запись `world.ini` |
| 40 | `WorldFilesDataTests` | палитры/`hardness.bin`/XML/текстура |
| 50 | `SaveWorldTests` | конвейер `saveWorld()` + перезагрузка |
| 53 | `UndoHistoryTests` | snapshot→edit→undo→redo байт-в-байт, отбрасывание redo-хвоста, клип прямоугольника, сброс истории, изоляция истории при переключении миров (T1.4) |
| 55 | `ViewportRendererTests` | растр == `getColor32`, грязные прямоугольники, фильтры Geo/Dam, оверлеи `GABuf` (T2.1) |

Тесты пишут только во временный content-root (`%TEMP%/map_editor_roundtrip`),
исходный мир не изменяется. `SaveWorldTests` строит изолированный content-root
(копия сохранённого мира + общие ресурсы `RESOURCE/GeoTx` + `Perimeter.ini`),
чтобы `fullLoad()` прошёл так же, как в игре.
