Да, логика работает в отдельном потоке — и ваше наблюдение точно соответствует тому, как это устроено в коде.

## Базовая схема

Заявлено прямо в шапке `Source/HT/ht.h:3-5`:

> «В одном потоке работает логика, в другом — графика. GameShell в графическом потоке.»

Реализация — `HTManager` (`Source/HT/ht.cpp`):

- `GameStart` создаёт поток `perimeter_logic_thread` (`ht.cpp:101-111`), но только если включён MT.
- Режим задаётся в `Perimeter.ini` ключом `[Game] HT` (по умолчанию **1**, т.е. многопоточность включена) или `-HT 0` в командной строке — `Source/Game/Runtime.cpp:1307-1310`.
- Поток логики получает `SDL_THREAD_PRIORITY_HIGH` (`ht.cpp:143`).
- `GameClose` останавливает его через семафор `end_logic` (`ht.cpp:118-139`).

В редакторе карт MT принудительно выключен: `Source/MapEditor/shim/game_stubs.cpp:72-82`.

## Что в каком потоке

**Логический поток** (`ht.cpp:141-203`): цикл `while(end_logic == nullptr)` → `gameShell->NetQuant()` → `LogicQuant()`. Внутри `LogicQuant` (`Runtime.cpp:439-471`) под `lock_logic` идут `profiler_quant`, `SoundQuant` и `gameShell->LogicQuant()` → `universe()->PrimaryQuant()`. Значит **вся симуляция, юниты, сквады, ИИ, A\* по кластерам и `DefenceMap` — в логическом потоке**.

**Графический поток** — главный цикл `Runtime::quant` → `HTManager::Quant()` (`ht.cpp:205-266`). В MT-режиме он делает только `GraphQuant()` и `gameShell->GraphQuant()`; в однопоточном (`HT=0`) тот же `Quant` крутит и логику, и графику инлайн.

## Как они синхронизируются (почему рендер не «залипает»)

1. **Фиксированный логический тик.** `terLogicTimePeriod = 100` мс (`Source/Scripts/Config.prm:14`) — 10 Гц. Логика крутится в `while(time < syncro_timer())`, накручивая `time += lag_stat->CalcDTime()`.
2. **Адаптивный dtime.** `LagStatistic::CalcDTime` (`Source/HT/LagStatistic.cpp:110-131`) при отставании снижает шаг до `0.5×100` мс, при простое — до `1.1×`. То есть при тяжёлой логике игра **замедляется**, а не копит долг. Плюс `if(time < syncro_timer()) time = syncro_timer()` — догон отбрасывается, а не отыгрывается.
3. **Интерполяция на рендере.** `GraphQuant` (`Runtime.cpp:473-492`) считает `interpolation_factor_ = clamp(interpolation_timer_ * terLogicTimePeriodInv, 0, 1)` и передаёт его в `gb_VisGeneric->SetInterpolationFactor`. `StreamInterpolator::ProcessData` (`Source/HT/StreamInterpolation.cpp:95-132`) в графическом потоке плавно интерполирует позиции/повороты/фазы между двумя последними логическими квантами. Поэтому при 10 Гц логики картинка остаётся плавной.
4. **Двойная буферизация UI-данных.** `LogicUpdater::exchange()` (`Source/UserInterface/LogicUpdater.cpp:32-36`) под мьютексом меняет `logicData`/`oldLogicData`; рендер читает `oldLogicData`, логика пишет в `logicData`.
5. **Списки объектов рендера по квантам.** `MTGVector::mtUpdate(cur_quant)` (`Source/Render/src/VisGrid2d.cpp:378+`) применяет `add_list`/`erase_list` только когда `quant <= cur_quant`, т.е. рендер не видит объект раньше, чем логика его «разрешила».
6. **Отложенное удаление юнитов.** `ClearDeleteUnit` (`ht.cpp:284-333`) держит объект живым ещё ~10 квантов и ждёт `GetGraphLogicQuant()`.
7. **Ассерты направленности.** `MTG()`/`MTL()` (`Source/Render/inc/RenderMT.h:16-17`) ловят вызовы рендер-API из логики и наоборот; `MTAutoSingleThread` (`ht.cpp:357-373`) временно разрешает оба типа.

## Почему «рендер не тормозит, а ИИ/логика — да»

Это и есть штатное поведение:

- Логика и рендер независимы по времени: рендер рисует последний доступный снимок состояния с интерполяцией, поэтому FPS держится даже при просадке логики.
- Просадка логики видна как **замедление игрового времени** (юниты едут медленнее, анимация медленнее), а не как падение FPS.
- В дебаге это видно по оверлею `LagStatistic::Show` (`lag_quant` / `net_skip` / `net_wait` / `speed_up`), включается `debug_show_lag_stat` и только в мультиплеере (`ht.cpp:347-354`).

Обратная зависимость тоже есть: логика **ждёт рендер**. В `ClearDeleteUnit` при MT стоит `while(gb_VisGeneric->GetGraphLogicQuant() < quant-wait_to_delete+2) Sleep(10);` — если рендер тормозит, логика встаёт. Так что при лагах рендера логика тоже просядет, но не наоборот.

## Где рендер всё-таки может ждать логику

Мест немного, и они в основном вне геймплея:

- `GameShell::setSpeed` (`GameShell.cpp:2569`), `switchActivePlayer` (`:2838`) — берут `GetLockLogic()` из UI.
- `terUniverse::universalLoad` (`Universe.cpp:794`) — загрузка/рестарт миссии.
- Меню/отладка: `MTAutoSingleThread` в `GameShell.cpp:675, 917, 1234`, `MultiplayerCommon.cpp:62, 166`, `MissionEdit.cpp:46, 97`.
- `cScene::Draw` берёт `lock_draw`, но это рендер-мьютекс, не логический.

Плюс нюанс приоритета: логический поток имеет `THREAD_PRIORITY_HIGH`, поэтому на слабом/загруженном CPU тяжёлая логика может отбирать кванты у рендера — но это конкуренция за CPU, а не блокировка на мьютексе.

## Как проверить у себя

- `Perimeter.ini`: `[Game] HT=0` — полностью однопоточный режим для сравнения поведения.
- `-HT 0` в командной строке.
- `-dump_mt_tls` — печатает, в каком потоке исполняется код (`VisGeneric.cpp:71-73`).