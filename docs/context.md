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
(`restoreRuntimeShadowBits`).
