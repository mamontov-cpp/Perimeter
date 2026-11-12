Коротко: **не совсем так**. В игре используется не сеточный `AIAStar`, а `AIAStarGraph` — A* по графу кластеров (`ClusterFind`). И применяется он не ко всем: **наземные боевые сквады — да, `BuildMaster` — нет**.

## Что реально используется

В `Source/AI/AIAStar.h` есть два класса:

- `AIAStar` — A* по клеточной сетке. **Он нигде не инстанцируется** — в `Source` нет ни одного `AIAStar<...>` кроме определений в самом заголовке.
- `AIAStarGraph` — A* по произвольному графу. Именно он работает через `ClusterFind` (`Source/AI/ClusterFind.h:63,88`), где узлы — кластеры карты, а не клетки.

То есть фактический pathfinding в игре — это **кластерный A***, а не клеточный.

## Сквады

Да, но с оговорками:

- `terUnitSquad::addWayPoint` (`Source/Units/Squad.cpp:1553-1572`) вызывает `ai_tile_map->findPath(...)`, только если отряд **не летающий** и в нём есть боевые юниты (`squadMutationMolecula().elementCount() > 0`). Иначе точка добавляется напрямую, без поиска пути.
- Внутри отряда юниты A* не считают: `terUnitLegionary::addWayPoint` просто кладёт точку в `out_path` (`Source/Units/IronLegion.cpp:376-379`). То есть один поиск на отряд, а не на каждого юнита.
- AI-логика дополнительно использует **другой** A* — по `DefenceMap` (карта плотности огня/хаоса/полей): `DefenceMap::findPathToPoint/findPathToTarget` (`Source/Units/DefenceMap.cpp:139-220`), вызовы из `Source/Units/Triggers.cpp:1041-1081`. Это планирование маршрута к цели/домой, а не движение конкретного юнита.

## BuildMaster

**Нет, A* он не использует.** `terUnitBuildMaster` наследуется от `terFrameChild` → `terUnitReal`, но в его атрибуте:

- `LegionType = LEGION_FLYING` (`Source/Scripts/UnitAttribute.prm:667`);
- `enablePathFind = 0` (`Source/Scripts/UnitAttribute.prm:679`).

Поэтому в `terUnitReal::moveToPoint` (`Source/Units/RealUnit.cpp:734-771`) ветка с `ai_tile_map->findPath` не выполняется — точка просто пушится в `way_points`. Движение идёт напрямую через `terFrameChild::WayPointController` (`Source/Units/FrameChild.cpp:41-85`).

`enableFieldPathFind = 1` у него стоит, но **чтения этого флага в `Source` я не нашёл** (только объявление, инициализация и конфиг). Класс `CalcPathFindFieldOp` (`RealUnit.cpp:774`) тоже нигде не вызывается — похоже на неиспользуемый/незавершённый код обхода силовых полей.

## «Только для наземных?»

По сути да:

- `AITileMap::findPath` (`Source/AI/AITileMap.cpp:225-251`) — единственный потребитель терраин-поиска.
- `enablePathFind = 1` в `UnitAttribute.prm` стоит только у `TerrainMaster`/Brigadier (`Source/Scripts/UnitAttribute.prm:595`) — наземный строитель.
- Наземные боевые сквады — через `Squad::addWayPoint`.
- Летающие и базовые отряды — без A*.
- `path_hard_map` с `PATH_HARD` создаётся, но `PATH_HARD` нигде не запрашивается — используется только `PATH_NORMAL`.

## Оптимизации

Да, их несколько уровней:

1. **Кластеризация — главная.** A* идёт по графу кластеров (`clusterSize = 10` клеток, `max_cluster_size = 8192`), а не по клеткам. Затем `SoftPath` разворачивает путь в клеточный и `SoftPath2`/`HeuristicLine` сглаживает его (`ClusterFind.h:57-104,184-277`).
2. **Разные карты и эвристики:** обычная (`walk`-стоимость + штраф 1000 при пересечении границы силового поля), `ClusterHeuristicDitch` (ров = 127 → штраф 1000), `ClusterHeuristicHard` (штраф ×10000 для сложного рельефа). Отдельные инстансы `path_finder`, `path_hard_map`, плюс `path_finder2`.
3. **Фоновая пересборка графа:** `SetLater`/`SetLaterQuant` с квантованием (`rebuildQuants = 10` для терраина, `50` для defence map) и двойным буфером со `swap` (`AITileMap.cpp:358-368`, `DefenceMap.cpp:28-40`).
4. **Кэш в юните:** путь пересчитывается не чаще раза в 1000 мс и только при смене цели — `pathFindTarget_`, `pathFindSucceeded_`, `recalcPathTimer_` (`RealUnit.cpp:743-751`).
5. **Bypass:** приказ с `COMMAND_SELECTED_MODE_OVERRIDE` идёт напрямую; летающие/пустые/базовые отряды тоже обходят A*.
6. **Multi-goal:** `FindPathMulti` + `ClusterHeuristicMulti` — один A* сразу на список целей, эвристика к их центру; плюс отсечение `ignorePathFindMinDistance2 = 200*200`, если цель и так близко (`DefenceMap.cpp:190`, `AI.prm:192`).
7. **Внутри `AIAStarGraph`:** generation-counter `is_used_num` вместо очистки массива (полный `clear()` только при переполнении счётчика), `multimap` как open-list, `self_it` для O(1)-удаления, несколько целей через `IsEndPoint`.
8. **Переиспользование AI-пути:** `findPathToTarget` возвращает путь, `Triggers` передаёт его в `squad->addTarget(target, path)`, и отряд нарезает его по радиусу огня (`lineCircleIntersection`, `Squad.cpp:1621-1646`) вместо нового поиска.

Отдельный нюанс: при возврате домой AI-путь по `DefenceMap` затем прогоняется через `Squad::addWayPoint` для каждой точки, а тот снова вызывает терраин-A*. То есть на этом маршруте поиск фактически выполняется дважды — по defence map и по terrain map.