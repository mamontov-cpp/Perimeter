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

## 5.7. `View3DRenderer` — наклонный 3D-вид (T2.3)

`core/View3DRenderer.{h,cpp}` (без Qt) — тонкая обёртка над `vrtMap::viewV2D`
(`Source/Terra/VMAP.cpp`): живой код Terra рисует объёмную карту сам, класс
только подготавливает параметры камеры и отдаёт готовый RGB565-буфер.

**Параметры камеры** (`View3DCamera`, всё в удобных единицах — градусы и
клетки):

| Поле | Смысл | Диапазон |
|---|---|---|
| `rotationDeg` | поворот вокруг Z (`turnAngleAroundZ`) | [0°, 360°) |
| `tiltDeg` | наклон камеры (`cameraAngleLean`) | инвертируется в `lean` |
| `liftCells` | высота камеры (`cameraDZ`) | `MIN/MAX_CAMERADZ_VX_WINDOW` |
| `focusCells` | «зум» (`focusC`) | [150, 8000] |

`lean` в `vrtMap` — целочисленный член; для его защиты от сингулярности в
`viewV2D` (деление на `d0 = |tan(lean)|*focusC`, когда сканируемые
дальностные радиусы попадают внутрь `[0, d0)`) служит
`stableLeanDeg(lean, focusC, yside)`: он отодвигает угол от вертикали так,
чтобы `d0` вышла за пределы сканируемого диапазона. `focusC` климпуется в
[150, 8000] — интервал, с которым `viewV2D` работает стабильно.

**Буфер.** Рендер `viewV2D` пишет RGB565 (`ColorsHalfType`), поэтому буфер
`View3DRenderer` объявлен как `ColorsHalfType` (uint16_t): раскладка
совпадает с `QImage::Format_RGB16`, виджет оборачивает его zero-copy —
зеркальное решение аргументации T2.1 (ARGB32 ↔ `Format_ARGB32`). Ширина
окна ограничена `MAX_XSIZE_VX_WINDOW` (640), массивы примера
`example_vx_window` уже статические, поэтому размер окна климпуется
`≤ 640×640`.

**Базис поворота.** `viewV2D` сканирует «трубу» перед камерой по клеткам;
направление «вперёд» при повороте `θ: (sin θ, −cos θ)` — это следует из
формул внутри `viewV2D`; панорамирование в виджете использует этот же базис
(вправо/влево от камеры = перпендикуляр).

## 5.8. 3D-режим в `ViewportWidget` + панель View (T2.3, GUI)

**ViewMode.** `ViewportWidget` имеет `ViewMode { TopDown, Oblique }`;
переключение (`setViewMode`) эмитит `viewModeChanged`. В Oblique виджет:

- оборачивает буфер `View3DRenderer` в `QImage::Format_RGB16` и рисует его
  целиком (базовая растеризация без зум-трансформаций — масштаб задаёт
  камера через `focusC`);
- правая/средняя кнопка — панорамирование по плоскости земли
  (`panObliqueBy`): сдвиг считается в клетках через базис камеры, затем
  переносится в `vMap.viewX/viewY`;
- левая кнопка — орбита (drag меняет поворот/наклон);
- колесо — зум `focusC` (шаг 10%);
- навигационные клавиши 2D-режима в 3D отключаются — камера в 3D
  самодостаточный навигатор.

**Панель View + меню.** `MainWindow::buildViewToolbar()` создаёт действия:
F9 — переключение 2D/3D, Ctrl+←/→ — поворот ±15°, Ctrl+↑/↓ — наклон ±5°,
PageUp/PageDown — подъём камеры ±100, Ctrl+= / Ctrl+- — зум, Ctrl+Home —
сброс камеры. Иконки нарисованы программно (`makeRotateIcon` и т.п.,
QPainter-дуги) — намеренно без `QPixmap::transformed`, который в связке с
QTransform вызывал падение CRT (см. `docs/context.md`). Живой QLabel в
панели показывает текущие значения (rot/tilt/lift/focus) и обновляется при
каждом изменении камеры.

## 5.9. Миникарта + прямоугольник вида (T2.4)

**Источники изображения.** `MinimapWidget` (dock-панель `MainWindow`, справа,
по умолчанию видима; переключается из меню View, клавиша M) показывает
`map.tga` текущего мира — тот же файл, что читает шелл игры; перечитывается
после `saveWorld()` (файл регенерируется при сохранении) и при загрузке мира.
Если `map.tga` отсутствует или не читается (свежий/сохранённый без него
мир) — панель рендерит живой растр через `WorldImages::renderToBgr()`, т.е.
тот же пиксельный источник, что и кэш вьюпорта (`getColor32`, Composite,
без оверлеев). Сам метод никогда не трогает Terra: пакеты `WorldImages`
(core, без Qt — TGA чтение/запись, даунскейл, генераторы `cache.tga`/
`map.tga`) переехали туда из `EditorSession` и переиспользуются конвейером
сохранения, поэтому сохраняемые файлы и миникарта не могут разойтись.

**Прямоугольник вида.** `ViewportWidget::currentViewRect()` — видимая область
карты в клетках: в TopDown это точный `visibleCellRange()`; в Oblique —
аппроксимация следа 3D-окна на земле (основание `viewV2D`: ширина ≈
окно × `(focusC+cameraDZ)/focusC`, глубина — через наклонный kv-коэффициент
`panObliqueBy`, после клипа по карте). Виджет эмитит `viewRectChanged`
после каждой отрисовки, где прямоугольник изменился (пан/зум/орбита/
переключение режима/resize/загрузка мира), — миникарта подписана на сигнал
и рисует жёлтую рамку. Клик по миникарте → `ViewportWidget::centerOnCell()`
(центрирует 2D-вид либо переносит 3D-`obliqueCenter_`).

## 5.10. Кисти — каркас (T3.1)

`core/BrushTool.{h,cpp}` (без Qt) — каркас для всех T3.x-инструментов:
`BrushParams{shape, radius, strength, mode(Geo/Dam), colorIndex, effect,
direction}` + генераторы масок `BrushMasks` + одна точка применения
`BrushTool::apply()`.

**Формы масок** (`BrushMask` — bbox-офсет относительно центра штампа +
`uint8 influence[0..255]` на клетку; частичные влияния появятся в T3.5):

| Форма | Геометрия |
|---|---|
| `FilledCircle` | все клетки с `ring(dx,dy) ≤ r` — то же множество, что штампует `influenceDZ` |
| `CircleOutline` | кольцо `ring == r` — ровно смещения собственных радиусных таблиц Terra `maxRad[xRad[r]/yRad[r]]` |
| `FilledSquare`/`SquareOutline` | `|dx|,|dy| ≤ r` / `max(|dx|,|dy|) == r` |
| `Line` | Брезенхэм от (x0,y0) до (x1,y1) со штампом-кругом радиуса `r`; каждая клетка входит в маску ровно один раз (перекрытия штампов не суммируются) |

`ring(dx,dy) = (int)sqrt(dx²+dy²)` — отбрасывающий `sqrt`, ровно тот же
округляющий расчёт, которым `vrtMap::landPrepare()` строит радиусные таблицы
(`LAND.cpp:77`), поэтому аналитические маски совпадают с таблицами Terra
клетка-в-клетку (проверено тестом 56). `sqrt` в игре — fdlibm-эквивалент
(`gamemath.h`/`std.cpp`), т.е. правильно-округлённый, как и `std::sqrt`.

**Применение** (`applyStamp`/`applyLine`/`applyMask`, один вызов = одна
undo-запись):

1. клип bbox по карте (торус не используется — как `recordUndoArea`);
2. `session.recordUndoArea(bbox)` ДО правок (план называет
   `UndoDispatcher_PutPreChangedArea`, но он пуст под `_SURMAP_` — контракт
   undo редактора описан в §4.1);
3. маршрутизация эффекта: пока реализован только
   `ChangeHeight` (поднять/опустить) — каждая клетка маски проходит через
   Terra-тулзу `individualToolzer<T2TE_CHANGING_TERRAIN_HEIGHT>::tVoxSet()`:
   hardness-затухание, биты `At_*`, согласованность `VxGBuf/VxDBuf/SurBuf`
   и разделение слоёв Geo/Dam — всё это поведение самой Terra, не
   переизобретается. Дельта клетки = `direction × strength` (в 1/32 м,
   `<<VX_FRACTION`) × влияние маски (255 → полный дельта), округление по
   модулю. Остальные эффекты (`Align4ZP`, `SetColor`, `PlotZeroplast`, …)
   объявлены в `BrushEffect`, `apply()` их отвергает до соответствующих
   задач T3.2–T3.5;
4. `vMap.recalcArea2Grid(bbox±1)` + `vMap.regRender(bbox±1)` — сетка и
   вьюпорт обновляются по обычной dirty-конвейерке (запас в 1 клетку, как
   у тулз игры, `tools.h:252-254`).

Значение `mode(Geo/Dam)` передаётся в параметры и станет значимым в T3.2
(сегодня высотная тулза Terra сама выбирает активный слой). `strength`
климпуется в `[1,100]`, `radius` — в `[0, MAX_RADIUS_CIRCLEARR]`.

**GUI-интеграция.** `MainWindow` владеет одним `BrushTool` (параметры —
пока дефолт: круг r=8, поднять, strength 30%); `ViewportWidget` при
`setBrushTool()`+`setBrushEditingEnabled()` красит по левому драгу в
TopDown-режиме (штамп на каждую новую клетку курсора), курсор — крест,
`viewRectChanged`/превью работают как раньше. Переключатель — действие
`actBrushEdit_` («Brush Editing», T) в меню Edit и в тулбаре; сигнал
`worldEdited()` обновляет статус-бар/undo-actions. Панель параметров
(`ToolPanel`) появится в T3.2 и переиспользует `setParams()`.

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
| 45 | `WorldImagesTests` | round-trip 24-битного TGA (запись→чтение), флап строк при bottom-left origin, отказ на сжатом/палитровом/обрезанном/отсутствующем файле, пиксели `downscaleNearest` (T2.4) |
| 55 | `ViewportRendererTests` | растр == `getColor32`, грязные прямоугольники, фильтры Geo/Dam, оверлеи `GABuf` (T2.1) |
| 56 | `BrushMaskTests` | маски совпадают с радиусными таблицами Terra, квадраты/кольца/Брезенхэм-линия, degenerate-штамп, routing `byShape` (T3.1) |
| 57 | `View3DRendererTests` | кадр 3D заполнен разумно (покрытие, число цветов), поворот на 90° меняет кадр, клип окна ≤ `MAX_XSIZE_VX_WINDOW`, инвариант `stableLeanDeg` (T2.3) |
| 58 | `BrushToolTests` | штамп меняет ровно клетки маски на `strength<<VX_FRACTION`, undo/redo байт-в-байт, линии по одной undo-записи, отказ неподдержанных эффектов, клипы radius/strength (T3.1) |

Тесты пишут только во временный content-root (`%TEMP%/map_editor_roundtrip`),
исходный мир не изменяется. `SaveWorldTests` строит изолированный content-root
(копия сохранённого мира + общие ресурсы `RESOURCE/GeoTx` + `Perimeter.ini`),
чтобы `fullLoad()` прошёл так же, как в игре.
