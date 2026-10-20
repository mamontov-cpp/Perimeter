# Контекст: нетривиальные находки (редактор карт)

Заметки о неочевидном поведении кода игры/Terra, важные для редактора.
Связанные архитектурные решения — в `docs/architecture.md`.

## Terra

### `vrtMap::prepare()` можно вызвать только один раз за процесс

`vrtMap::prepare()` (`Source/Terra/VMAP.cpp:484`) вызывает `landPrepare()`
(`Source/Terra/LAND.cpp:64`), которая:

- **накапливает** глобальную таблицу `maxRad[MAX_RADIUS_CIRCLEARR+1]` (не
  обнуляется перед подсчётом), и
- **течёт**: аллоцирует `static_xheap`/`static_yheap`, но не освобождает
  предыдущие (освобождает только `landRelease()`, которую `prepare` не зовёт).

Итог: второй вызов `prepare()` в том же процессе считает `max` по удвоенным
`maxRad` и переполняет заново выделенные heap-массивы `xheap/yheap` →
повреждение памяти/краш (0xC0000005). Первый вызов безопасен.

**Что делает редактор.** `EditorSession::refreshWorldTable()` вызывает
`vMap.prepare("RESOURCE/Worlds")` один раз, а дальше сам перестраивает
`vMap.wTable` (та же переборка каталогов `RESOURCE/Worlds`, что и внутри
`prepare`, но без `landPrepare`).

**Почему нельзя полагаться на собственный флаг.** Другие вызовы (игра, тесты)
могут дёрнуть `vMap.prepare()` напрямую, минуя редактор. Поэтому «уже
подготовлено» определяется по состоянию самой Terra: `landPrepare()` оставляет
`xRad[0] != nullptr` (глобальные `xRad/yRad/maxRad` объявлены в
`Source/Terra/tools.h:12-14`). Хелпер `terraLandPrepared()` проверяет `xRad[0]`.

**Побочный эффект.** `vrtMap::~vrtMap` и `EditorSession::unloadWorld()`
намеренно **не** вызывают `landRelease()` — иначе радиусные таблицы пропадут и
понадобится повторный `prepare()`, который крашится (см. выше).

### `worldFree()` — не полная выгрузка мира

`vrtMap::worldFree()` (`Source/Terra/worldGen.cpp:485`) освобождает только
сеточные карты (`r_net_map` и т.п.) и обнуляет `r_net_map`. Буферы
`VxGBuf/VxDBuf/SurBuf/AtrBuf/RnrBuf/SupBuf/GVBuf/GABuf` остаются.

Полная уборка — в `vrtMap::~vrtMap` (`Source/Terra/VMAP.cpp:247`):
`delLeveledTexture()` → `releaseMem4Buf()` → `wTable.clear()` →
`landRelease()` → `worldFree()`. Порядок важен: `delLeveledTexture()` должен
идти раньше, чем `releaseMem4Buf()` (комментарий в коде: «Необходимо вызывать
до удаления VxDBuf!»), иначе `LvdTex` может совпадать с `SurBuf`.

`EditorSession::unloadWorld()` повторяет эту последовательность, кроме
`landRelease()`.

### `save3Buf()` уже включает палитры и `[3DF]`

`vrtMap::save3Buf()` (`Source/Terra/VMAP.cpp:1514`) вызывает `save3BufOnly()`
(пишет `output.vmp` в формате S2T0), затем `saveGeoDamPal()`
(`inGeo.act`/`inDam.act`) и `f3d.saveVariable()` (`[3DF Parameters]` в
`world.ini`). Редактору достаточно `save3Buf()` — отдельные вызовы не нужны.

### `cache.tga` обязателен при `fullLoad()`

`fullLoad()` `xassert`-ит на `cache.tga` (`Source/Terra/VMAP.cpp:935-941`).
Файл не поставляется с игрой, поэтому редактор генерирует его на каждом
сохранении (24-бит, `H_SIZE×V_SIZE`). Тест `WorldRoundTripTests` на время
round-trip делает фиктивную чёрную `cache.tga`, если её нет.

### Ссылки на общие ресурсы из XML мира

`geoTx.xml` мира ссылается на общие текстуры вида
`Resource\GeoTx\Stone_2.tga`. Это **не** файлы внутри папки мира, а общий
ресурс `RESOURCE/GeoTx`. Поэтому мир нельзя загрузить из content-root, в
котором есть только папка мира — нужен ещё `RESOURCE/GeoTx` (и `Perimeter.ini`
для `[TD] Color ZeroLayer`, читаемого `analyzeINI`). `SaveWorldTests` строит
такой изолированный content-root, чтобы не трогать папку игры.

### `wTable[].key` — это относительный путь, а не имя папки

В `get_content_entries_directory` поле `key` записи — нижнерегистровый
относительный путь (`resource\worlds\compass`), а имя папки — это
`path_content.filename()` (`compass`). При построении путей к файлам мира
(`GetTargetName` = `wTable[n].dir` + имя файла) нужен именно полный ключ;
при создании каталогов по имени мира — имя папки, не ключ.

### Палитры цвета: `RnrBuf` и общая индексация `SurBuf`

`RnrBuf[off]` (`LAND.cpp:1461-1462`): бит 7 = «здесь дамба» (ставится тогда,
когда `VxDBuf != 0`), биты 0..6 — освещение (`свет >> 1`, всегда ≤127).
`Sur2Col32[Sur][Rnr]` (`VMAP.h:321`) — вторая размерность загоняется дважды:
`convertPal2TableSurCol(GeoPal, Geologic)` заполняет строки 0..127
(`GeoPal`), `convertPal2TableSurCol(DamPal, Damming)` — строки 128..255
(`DamPal`), при этом **первый** индекс (индекс материала в `SurBuf`) общий
для обеих палитр. `PutAltDam()` при установке дамбы **не** переписывает
`SurBuf` (`SetTer` закомментирован, `VMAP.h:864`) — индекс материала
сохраняется. Отсюда: цвет дамбы = `DamPal[Sur] × освещение`, а «гео под
дамбой» восстанавливается точно как `Sur2Col32[Sur][Rnr & 0x7F]` — этим
пользуется Geo-фильтр `ViewportRenderer` (без аппроксимаций через
`GetGeoType`). Ветки `At_LEVELED` ведут себя так же: `Tex2Col32`/`SurZP2Col32`
тоже строятся двумя проходами и берут тот же `Rnr`.

`getColor32` в release-сборке не имеет debug-тинтов: `flag_ShowHardness` /
`flag_ShowDbgInfo` инициализируются нулём в конструкторе (`VMAP.cpp:220-221`).

### Конвейер грязных прямоугольников `regRender → renderQuant`

- `regRender()` (`LAND.cpp:892`) `XCYCL`-ит координаты и, если прямоугольник
  «заворачивается» через границу, дробит его на 2-4 бокса; каждый бокс
  уходит в `renderBox`, который пушит **один и тот же** прямоугольник сразу в
  `vMap.renderAreas` и `vMap.changedAreas`, а для вырожденного запроса
  (`x1==x2` или `y1==y2`) расширяет бокс до **всей строки/колонки**
  (`LAND.cpp:693-696`).
- `sRectS::dx/dy` — **включительные** экстенты (`x1() = x + dx`);
  `renderQuant()` перерисовывает строки `y..y+dx` включительно.
- `renderQuant()` (`LAND.cpp:760`) ранний выход при пустом `renderAreas`,
  в конце делает `renderAreas.clear()` (`LAND.cpp:884`), а `changedAreas`
  **не** трогает — их чистит потребитель: в игре это `terUniverse::quant()`
  (`Universe.cpp:250`), в редакторе — `ViewportRenderer::update()`
  (иначе список растёт бесконечно, а растровый кэш отстаёт).
- `renderBox` дополнительно выставляет `changedT[y]` — ветке `PERIMETER`
  это ни на что не влияет (`changedT` читают другие пути рендера).

### `RnrBuf` не вычисляется при загрузке мира

`fullLoad()` распределяет `RnrBuf`, но цикл `RenderStr` внутри него
**закомментирован** (`VMAP.cpp:924-928`) — буфер приходит в мусорном
состоянии. Игра после загрузки вызывает `vMap.WorldRender()`
(`terUniverse::universalLoad`, `Universe.cpp:1052`), который пересчитывает
`RnrBuf` и `At_SHADOW` по всем строкам. Редактор делает то же самое в
`EditorSession::loadWorld()` (п.5) — до этой правки любой `getColor32()`
(вьюпорт, генерация `cache.tga`) читал неинициализированное освещение.

## Файлы мира

### `output.vmp`: legacy-поля заголовка не перезаписываются

`sVmpHeader` содержит поля `NX/NY` (байты 12..19), которые runtime не читает.
`save3BufOnly()` их не заполняет, поэтому для байт-идентичного round-trip
редактор/тест копирует эти 8 байт из исходного файла
(`preserveVmpLegacyHeader` в `WorldRoundTripTests`). Игровая сборка сама миры
не пересохраняет, так что это забота редактора.

### Флаг `At_SHADOW` (0x20) — runtime-only

`fullLoad()` → `checkAndRecover()` очищает `At_SHADOW` во всех клетках
`AtrBuf`; тени пересчитывает рендер. Чтобы «загрузить и сохранить» давало
исходный файл байт-в-байт, тест возвращает эти биты из исходного `output.vmp`
(`restoreRuntimeShadowBits`). В `COMPASS`, например, 869 733 клетки имеют
`At_SHADOW`, «зашитый» в файле.

Нюанс: `EditorSession::loadWorld()` теперь вызывает `WorldRender()`
(см. `docs/architecture.md`, §2), поэтому мир, **сохранённый через
`EditorSession`**, пишет вычисленные тени, а не исходные биты файла. Байтовая
стабильность round-trip от этого не страдает: `WorldRoundTripTests` грузит мир
через сырой Terra-API (без `WorldRender`) и восстанавливает тени явно, а
`SaveWorldTests` `output.vmp` не байт-сравнивает.

### Некопаемость: семантика `hardness.bin` и `GABuf`

- Младшие 2 бита ячейки `GABuf` (`GRIDAT_MASK_HARDNESS`): **0 = некопаемо**
  (миссионный редактор пишет 0 именно для «сделать твёрдым»,
  `MissionEdit.cpp:641`; `tVoxSet` масштабирует копание как `h<<h`/24, т.е.
  при 0 копать нельзя), **3 = нормально копаемый грунт** (дефолт
  `initGrid()`). Значения 1/2 формат допускает, но в поставляемых данных
  их нет: декод `hardness.bin` всех миров даёт только 0 и 3 (COMPASS:
  28 692×0 / 233 452×3; HILLKAN: 226 879×0 / 35 265×3; INFORB-1 и VENECIDE —
  только 0).
- Файл: `uint16` ширина сетки (`GH_SIZE`), `uint16` высота, затем байты по
  4 ячейки (по 2 бита, младшие — первые).
- **Миссия vs битва.** Игра после загрузки мира делает
  `vMap.initGrid()` (вся некопаемость → 3) и `loadHardness()` **только** если
  из `.spg` пришло `manualData.loadHardness` (`Universe.cpp:949-952`); у
  многих `RESOURCE/Multiplayer/*`-карт это `false`, у `Missions/*` и
  `Worlds/*.spg` — `true`. Редакторский `fullLoad()` применяет `hardness.bin`
  безусловно, поэтому оверлей «некопаемость» показывает **истину мира-файла**;
  нюанс пропуска `loadHardness` из `.spg` относится к задачам T8/T9.
- `initGrid()` (`grid.cpp:45-90`) для каждого 4×4-блока ставит флаги:
  `GRIDAT_LEVELED` — только если все 16 клеток `At_LEVELED`,
  `GRIDAT_ZEROPLAST` — все 16 зеропласт, `GRIDAT_ZEROPLAST_IS_PRESENT` —
  хотя бы одна. Клетки с дамбой в счётчики не входят, но `At_ZEROPLAST`
  (0xC0) включает бит `At_LEVELED` (0x80), поэтому «полный зеропласт» всегда
  заодно и «выровнен» — отсюда приоритет зеропласт-тинта в оверлее.
- `At_NOTPURESURFACE == 0xC0 == At_ZPMASK` (`terra.h:46`): `SPutAltDam()`
  чистит флаги выровненности/зеропласта вместе с дробной частью высоты —
  при синтезе дамбы в тесте клетка гарантированно уходит из ветки
  `At_LEVELED`.

### В поставляемых мирах нет клеток `At_LEVELED` / `At_ZEROPLAST`

Прямой декод `AtrBuf` из `output.vmp` (COMPASS, HILLKAN, INFORB-1, ALPAIN,
KWILL, MAWN, XELA): верхние 2 бита не выставлены **ни у одной** клетки —
цветовые ветки `Tex2Col32`/`SurZP2Col32` в загруженных мирах не
встречаются, они оживаются только после правок (буши, зеропласт в миссиях).
Поэтому `ViewportRendererTests` выставляет эти флаги сам (форс-ячейки) и
проверяет ветки `getColor32` на реальных палитрах мира.

### Топология COMPASS: слои большими пятнами, гео — низкое

Декод `output.vmp` COMPASS (2048²): ~47% клеток с дамбой, причём области
дамбы/гео идут крупными блоками (опрос тайлами 128²: x 768..1023 /
y 256..511 — 100% гео, (256..384)² — 100% дамба). Целая часть высоты
**гео**-слоя лежит в 0..31 — «поверхность», которую видит игрок на картах со
многими дамбами, хранится в слое дамб. Полезно для выбора опорных клеток в
тестах рендера (боксы сканов захардкожены в `ViewportRendererTests` с
ссылкой на этот раздел).
