Враги-«скверна» (scourge/filth) в Perimeter реализованы как система «спот → рой → юниты». Вот полный список файлов и типов.

## Архитектура (общая инфраструктура)

| Файл | Роль |
|---|---|
| `Source/Units/GenericFilth.h/.cpp` | Базовые классы: `terFilthGeneric`, `terFilthSpline`, `terFilthSwarm` и главный юнит **`terFilthSpot`** («гнездо», из которого рождаются рои). Вся логика спавна — в `GenerationProcess()` (switch по `FILTH_SPOT_ID_*`) |
| `Source/Game/Player.cpp` (строки 637–662) | Фабрика `terCreateUnitData` — создание юнитов скверны по `UNIT_CLASS_ID_FILTH_*` |
| `Source/Scripts/UnitAttribute.inl` (522–549) | Перечень всех видов: enum **`terFilthSpotID`** (`FILTH_SPOT_ID_ANTS`, `_WASP`, `_DRAGON`, `_GHOST`, `_EYE`, `_CROW`, `_DAEMON`, `_RAT`, `_WORM`, `_SHARK`, `_VOLCANO`, `_ANTS2`, `_SNAKE`, `_DRAGON2`, `_VOLCANO_SCUM_DISRUPTOR` + «новые» ET-варианты `A_*`) |
| `Source/Scripts/GameBalance.inl` (829–1017) | Баланс: `MobileUnit Ants, Ants2, Rat, Snake, Worm, Wasp, Ghost, Eye, Crow, Daemon, DragonHead/Body/Tail, Dragon2*, Shark, Volcano, VolcanoScumDisruptor`; плюс спец-юниты Harkbakchood `Scourge0–3` (749–826) |
| `Source/UserInterface/MissionEdit.cpp` (271–319) | Меню редактора миссий со списком всех видов («дракон», «муравей», «пчела», …, «рыба», «вулкан»…) |
| `Source/Util/Save.h`, `Source/Util/SaveOthers.cpp` | Сохранение: `SaveFilthSpot`, `SaveFilthSwarm*`, `SaveFilth*` + регистрация enum'ов |

## Конкретные враги (файлы реализации)

Каждый вид — пара файлов в `Source/Units/`:

| Файл | Классы | Вид скверны |
|---|---|---|
| `FilthAnts.h/.cpp` | `terFilthAnt`, `terFilthSwarmAnt` | **Муравьи** (`ANTS`, `ANTS2`) и **«паук»/spider** — это `FILTH_SPOT_ID_A_SPIDER` (модели `aspider.M3D`), обрабатывается тем же классом |
| `FilthShark.h/.cpp` | `terFilthShark`, `terFilthSwarmShark` | **«Рыба»/акула** (`SHARK`, «Scourge Shark») — движется по сплайну, всплывает из-под земли |
| `FilthWasp.h/.cpp` | `terFilthWasp`, `terFilthSwarmWasp` | **Осы** (`WASP`, `A_WASP`) + «глаза» через `SetPrm` |
| `FilthEye.h/.cpp` | `terFilthEye`, `terFilthSwarmEye` | **Глаза** (`EYE`, `A_EYE`) — выстреливают снарядами |
| `FilthGhost.h/.cpp` | `terFilthGhost`, `terFilthSwarmGhost` | **Приведения** (`GHOST`) — атакуют лазером |
| `FilthCrow.h/.cpp` | `terFilthCrow`, `terFilthSwarmCrow` | **Вороны** (`CROW`, `A_CROW`) — воздушные, сбрасывают бомбы |
| `FilthDaemon.h/.cpp` | `terFilthDaemon`, `terFilthSwarmDaemon` | **Демоны** (`DAEMON`, `A_DAEMON`) |
| `FilthDragon.h/.cpp` | `terFilthDragonHead`, `terFilthDragonBody`, `terFilthSwarmDragon` | **Дракон** (`DRAGON`, `DRAGON2`, `A_DRAGON`) — цепочка голова/тело/хвост, самый сложный враг |
| `FilthRat.h/.cpp` | `terFilthRat`, `terFilthSwarmRat` | **Крысы** (`RAT`, `A_RAT`) и **«змея»/Scourge Spider** — это `FILTH_SPOT_ID_SNAKE` (см. `GameBalance.inl:745-747`) |
| `FilthWorm.h/.cpp` | `terFilthWorm`, `terFilthSwarmWorm` | **Червь** (`WORM`, `A_WORM`) — подземный, грызёт здания |
| `FilthVolcano.h/.cpp` | `terFilthVolcano`, `terFilthSwarmVolcano` | **Вулкан** (`VOLCANO`, `VOLCANO_SCUM_DISRUPTOR`) — спавнит лаву/влияние на геометрию |

## Итого: полный список врагов скверны

1. Муравьи (ants)
2. Паук (spider, «муравьи2» / ET-модель aspider)
3. Рыба/акула (fish/shark)
4. Осы (wasp)
5. Глаза (eye)
6. Приведения (ghost)
7. Вороны (crow)
8. Демоны (daemon)
9. Дракон (dragon, 2 варианта + ET-модель)
10. Крысы (rat)
11. Змея/Scourge Spider (snake — спец-юнит Harkbakchood)
12. Червь (worm)
13. Вулкан (volcano, включая вариант от scum disruptor)

Помимо этого есть «сквад-скверна» — юниты игрока `UNIT_ATTRIBUTE_FILTH_SPOT0..3` (муравьи/рыба/осы/червяк), которые сами создают споты скверны (`ShellCmd.cpp:949–960`, `UnitAttribute.inl:144–147`).