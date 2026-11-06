# Формат M3D в PluginMAX

Ниже — описание формата по коду `Source/PluginMAX` и по тому, как его читает игра (`Source/Render/src/ObjLibrary.cpp`). M3D — это не формат 3ds Max напрямую, а промежуточный бинарный формат сцены/модели: иерархия блоков и полей, little-endian, без выравнивания.

Основные файлы:

- `Source/PluginMAX/Inc/meshfile.h` — заголовок, общие типы, кодирование массивов.
- `Source/PluginMAX/Inc/DefMeshFile.h` — ID блоков, ID полей, типы значений.
- `Source/PluginMAX/Src/DefMeshFile.cpp` — таблицы имён/размеров.
- `Source/PluginMAX/SceneMesh.h` — структуры сцены, материалов, объектов, анимации.
- `Source/PluginMAX/Src/*.cpp` — Read/Write каждого блока.
- `Source/Render/src/ObjLibrary.cpp` — реальный загрузчик M3D в игре.
- `Source/PluginMAX/m3d.txt` — заметка про привязку эффектов к нодам.

---

## 1. Заголовок файла

Заголовок пишется в `cMeshFile::WriteHeaderFile` и читается в `ReadHeaderFile` (`Source/PluginMAX/Src/MeshFile.cpp:72-100`).

| Смещение | Размер | Поле | Значение |
|---:|---:|---|---|
| 0 | 4 | идентификатор | `'m'`, `'3'`, `'d'`, `0` — строка `"m3d"` вместе с нулевым терминатором |
| 4 | 1 | тип файла | `'b'` (`0x62`) — бинарный, `'t'` (`0x74`) — текстовый |
| 5 | 4 | версия | `int32` little-endian; максимальная версия — `3` |

Итого **9 байт**.

Важные детали:

- Идентификатор записывается через `sizeof(MESHFILE_IDENTIFIER)`, то есть **4 байта вместе с `\0`**, а не 3.
- Версия — обычный 4-байтовый `int`, не один байт.
- Формально поддерживается текстовый режим `'t'`, и writer умеет его писать, но текущий reader для текстовых полей содержит `assert(0)` (`MeshFile.cpp:102-156`). Игра использует только `'b'`.
- Для версии 1 ID полей и блоков занимают **2 байта**, для версий 2 и 3 — **1 байт**.

---

## 2. Общий принцип: поля и блоки

После заголовка идёт последовательность полей. Для версий 2/3:

```text
field_type : 1 байт
payload    : зависит от field_type
```

Если `field_type == 0` (`MF_TYPE_BLOCK`, `$BLOCK`), то дальше идёт:

```text
block_id : 1 байт
```

Для версии 1 и `field_type`, и `block_id` — по 2 байта.

### 2.1. Блоки

Блок не имеет длины. Он открывается и закрывается **одним и тем же** `block_id`:

```text
[0][block_id]   ... содержимое ...
[0][block_id]   закрытие
```

Поэтому парсер узнаёт о закрытии блока, когда внутри него встречает block с тем же ID. Блоки могут быть вложенными, но с разными ID.

### 2.2. Поля-значения

Поле пишется как:

```text
[field_type][значение]
```

Для массивов тип пишется **один раз**, а затем идёт `N` элементов подряд:

```text
[field_type][element_0][element_1]...[element_N-1]
```

Размер элемента берётся из `GlobalTypeVariable` (`DefMeshFile.cpp:100-124`).

### 2.3. Строки

Строки не имеют длины. Они пишутся в кавычках:

```text
[field_type]["some text"]
```

Пустая строка — `""`. Внутри строк не должно быть кавычек: reader просто ищет следующую `"`.

### 2.4. Геометрические массивы

Массивы геометрии и анимационных ключей кодируются блоком:

```text
[0][block_id]          открытие массива
[11][uint32 N]         поле Number — количество элементов
[field_type][N * sizeof(element)]
[0][block_id]          закрытие массива
```

Пример для `VERTEX_MESH`:

```text
00 0C          $BLOCK VERTEX_MESH
0B 34 12 00 00 Number = 0x1234
13             Vertex (float3)
...            0x1234 * 12 байт
00 0C          $BLOCK VERTEX_MESH (закрытие)
```

### 2.5. Библиотеки объектов

Для `MATERIAL`, `OBJECT`, `ANIMATION_MESH`, `CHANNEL_MAPPING`, `SUBTEXMAP`, `LOD`, `CHANNEL` и т.п. **count не пишется**. Они идут подряд, каждый своим блоком, до закрытия родительского блока:

```text
[0][MATERIAL] ... [0][MATERIAL]
[0][MATERIAL] ... [0][MATERIAL]
...
[0][LOD]       закрытие родителя
```

Это важно: количество материалов/объектов/анимационных мешей определяется не отдельным полем, а количеством встреченных блоков.

### 2.6. Порядок байт и выравнивание

Данные пишутся через `memcpy` в `cStream` (`Source/PluginMAX/Src/StreamBuffer.cpp`), без преобразования порядка байт и без выравнивания. На практике формат рассчитан на x86:

- `int32`, `uint32`, `float` — 4 байта little-endian;
- `int16`, `uint16` — 2 байта little-endian;
- `float2/3/4/5/12/13` — просто несколько `float` подряд.

---

## 3. ID полей

Таблица находится в `Source/PluginMAX/Src/DefMeshFile.cpp:3-58`. ID поля — это индекс в `GlobalFieldType`.

| ID | Имя | Payload | Назначение |
|---:|---|---|---|
| 0 | `$BLOCK` | — | Служебный тип блока |
| 1 | `Comment` | string | Комментарий (в объектных блоках не разбирается) |
| 2 | `ID` | uint32 | Идентификатор |
| 3 | `Name` | string | Имя |
| 4 | `FileName` | string | Имя файла (текстура, projection) |
| 5 | `AmbientColor` | float3 | Цвет ambient (старый формат) |
| 6 | `DiffuseColor` | float3 | Цвет diffuse (старый формат) |
| 7 | `SpecularColor` | float3 | Цвет specular (старый формат) |
| 8 | `FloatVar1` | float | Скаляр 1 (shininess и т.п.) |
| 9 | `FloatVar2` | float | Скаляр 2 (shininess strength и т.п.) |
| 10 | `FloatVar3` | float | Скаляр 3 (transparency и т.п.) |
| 11 | `Number` | uint32 | Количество элементов массива |
| 12 | `KeyVisibility` | float2 | Ключ видимости: `time, visible` |
| 13 | `KeyPosition` | float4 | Ключ позиции: `time, x, y, z` |
| 14 | `KeyRotation` | float5 | Ключ поворота: `time, x, y, z, w` |
| 15 | `KeyScale` | float4 | Ключ масштаба: `time, sx, sy, sz` |
| 16 | `Parent` | string | Имя родительской ноды |
| 17 | `Matrix` | float12 | Матрица 3×4 (3×3 rotation + translation) |
| 18 | `NumberSubObject` | uint32 | Число подматериалов / тип объекта |
| 19 | `Vertex` | float3 | Вершина |
| 20 | `Face` (старый) | int4 | Грань версии 1: 3 индекса + материал |
| 21 | `FirstFrame` | uint32 | Первый кадр канала |
| 22 | `LastFrame` | uint32 | Последний кадр канала |
| 23 | `FrameSpeed` | uint32 | Скорость кадров |
| 24 | `TicksPerFrame` | uint32 | Тиков на кадр |
| 25 | `Time` | uint32 | Время ключа |
| 26 | `LOD` | uint32 | Уровень детализации |
| 27 | `TexVertex` (старый) | float3 | Текстурная вершина версии 1 |
| 28 | `TexFace` (старый) | int3 | Текстурная грань версии 1 |
| 29 | `ChannelNumber` | uint32 | Номер UV-канала |
| 30 | `HelperName` | string | `"Dummy"` / `"Group"` |
| 31 | `NumberFrame` | uint32 | Число кадров анимации |
| 32 | `Face` | ushort4 | Грань: `i0, i1, i2, materialIndex` |
| 33 | `TexVertex` | float2 | UV-вершина: `u, v` |
| 34 | `TexFace` | ushort3 | Текстурная грань: 3 индекса |
| 35 | `ScaleRotation` | float5 | Ключ scale-rotation: `time, x, y, z, w` |
| 36 | `Shading` | uint32 | Тип материала |
| 37 | `Matrix1` | float12 | Дополнительная матрица (у света — `OfsMatrix`) |
| 38 | `IntVar1` | int32 | Целое 1 (`SelfIllumColorOn`, `UseGlobal`) |
| 39 | `AmbientAnimErr` | float2 | Устаревший ключ ambient |
| 40 | `DiffuseAnim` | float4 | Ключ diffuse: `time, r, g, b` |
| 41 | `SpecularAnim` | float4 | Ключ specular: `time, r, g, b` |
| 42 | `MatrixAnim` | float13 | Ключ UV-матрицы: `time + 12 float` |
| 43 | `FloatVar1Anim` | float2 | Ключ shininess: `time, value` |
| 44 | `FloatVar2Anim` | float2 | Ключ shininess strength: `time, value` |
| 45 | `FloatVar3Anim` | float2 | Ключ transparency: `time, value` |
| 46 | `AmbientAnim` | float4 | Ключ ambient: `time, r, g, b` |
| 47 | `EmissiveAnim` | float4 | Ключ emissive: `time, r, g, b` |
| 48 | `IntVar2` | int32 | Целое 2 (`Falloff`) |
| 49 | `BlendType` | int32 | Режим смешивания |
| 50 | `BumpScale` | float | Масштаб bump для сцены |

Особенность: ID 20/27/28 — старые «широкие» форматы геометрии, ID 32/33/34 — новые компактные. В таблице они стоят на разных позициях, поэтому парсер различает их по фактическому ID поля.

---

## 4. ID блоков

Таблица находится в `Source/PluginMAX/Inc/DefMeshFile.h:4-39` и `DefMeshFile.cpp:60-98`.

| ID | Блок | Назначение |
|---:|---|---|
| `0x00` | `NONE` | Зарезервирован |
| `0x01` | `SCENE` | Корень M3D-сцены |
| `0x02` | `LOD` | Уровень детализации |
| `0x03` | `CHANNEL` | Канал анимации |
| `0x04` | `MATERIAL` | Материал |
| `0x05` | `SUBTEXMAP` | Текстурная карта материала |
| `0x06` | `OBJECT` | Mesh-объект |
| `0x07` | `ANIMATION_VISIBILITY` | Анимация видимости |
| `0x08` | `ANIMATION_POSITION` | Анимация позиции |
| `0x09` | `ANIMATION_ROTATION` | Анимация поворота |
| `0x0A` | `ANIMATION_SCALE` | Анимация масштаба |
| `0x0B` | `ANIMATION_MESH` | Состояние mesh для морфинга/анимации |
| `0x0C` | `VERTEX_MESH` | Массив вершин |
| `0x0D` | `FACE_MESH` | Массив граней |
| `0x0E` | `CHANNEL_MAPPING` | UV-канал |
| `0x0F` | `TEXVERTEX` | Массив UV-вершин |
| `0x10` | `TEXFACE` | Массив UV-граней |
| `0x11` | `VERTEXNORMAL` | Нормали вершин |
| `0x12` | `FACENORMAL` | Нормали граней |
| `0x13` | `HELPER_OBJECT` | Dummy/Group |
| `0x14` | `ANIMATION_MATERIAL` | Старый блок анимации материала |
| `0x15` | `LOGIC` | Логическая сцена (в текущем рантайме не используется) |
| `0x16` | `ANIMATION_SCALEROTATION` | Анимация scale-rotation |
| `0x17` | `LIGHT_OBJECT` | Источник света |
| `0x18` | `ANIMATION_LIGHT` | Ключ анимации света |
| `0x19` | `CAMERA_OBJECT` | Камера |
| `0x1A` | `ANIMATION_AMBIENT` | Анимация ambient |
| `0x1B` | `ANIMATION_DIFFUSE` | Анимация diffuse |
| `0x1C` | `ANIMATION_SPECULAR` | Анимация specular |
| `0x1D` | `ANIMATION_FLOAT_VAR1` | Анимация shininess |
| `0x1E` | `ANIMATION_FLOAT_VAR2` | Анимация shininess strength |
| `0x1F` | `ANIMATION_FLOAT_VAR3` | Анимация transparency |
| `0x20` | `ANIMATION_MATRIX` | Анимация UV-матрицы |
| `0x21` | `ANIMATION_EMISSIVE` | Анимация emissive |

---

## 5. Иерархия блоков

```text
SCENE
├─ ID, Name, BumpScale
└─ CHANNEL*
   ├─ ID, Name, FirstFrame, LastFrame, FrameSpeed, TicksPerFrame, NumberFrame
   └─ LOD*
      ├─ ID, LOD
      ├─ MATERIAL*
      │  ├─ ID, Name, Shading, Parent, NumberSubObject, IntVar1, IntVar2, BlendType
      │  ├─ ANIMATION_AMBIENT*
      │  ├─ ANIMATION_DIFFUSE*
      │  ├─ ANIMATION_SPECULAR*
      │  ├─ ANIMATION_EMISSIVE*
      │  ├─ ANIMATION_FLOAT_VAR1* (shininess)
      │  ├─ ANIMATION_FLOAT_VAR2* (shininess strength)
      │  ├─ ANIMATION_FLOAT_VAR3* (transparency)
      │  └─ SUBTEXMAP*
      │     ├─ ID, FileName, ChannelNumber
      │     └─ ANIMATION_MATRIX*
      └─ OBJECT* | HELPER_OBJECT* | LIGHT_OBJECT* | CAMERA_OBJECT*
         ├─ ID, Name, Parent, Matrix
         ├─ ANIMATION_VISIBILITY*
         ├─ ANIMATION_POSITION*
         ├─ ANIMATION_ROTATION*
         ├─ ANIMATION_SCALE*
         ├─ ANIMATION_SCALEROTATION*
         ├─ OBJECT:
         │  └─ ANIMATION_MESH*
         │     ├─ ID, Time
         │     ├─ VERTEX_MESH
         │     ├─ FACE_MESH
         │     ├─ VERTEXNORMAL
         │     ├─ FACENORMAL
         │     └─ CHANNEL_MAPPING*
         │        ├─ ID, Number
         │        ├─ TEXVERTEX
         │        └─ TEXFACE
         ├─ LIGHT_OBJECT:
         │  └─ ANIMATION_LIGHT*
         └─ CAMERA_OBJECT:
            └─ (без дополнительных блоков)
```

---

## 6. Подробно по блокам

### 6.1. `SCENE` — корень модели

Читается/пишется в `Source/PluginMAX/SceneMesh.cpp:11-55`.

```text
SCENE:
  ID        uint32
  Name      string
  BumpScale float     (может отсутствовать; по умолчанию 1.0)
  CHANNEL*  blocks
```

`BumpScale` — глобальный масштаб bump-текстур для этой модели. В рантайме применяется через `GetTexLibrary()->SetCurrentBumpScale()`.

### 6.2. `CHANNEL` — канал анимации

`Source/PluginMAX/Src/ChannelAnimation.cpp:3-62`.

```text
CHANNEL:
  ID            uint32
  Name          string
  FirstFrame    uint32
  LastFrame     uint32
  FrameSpeed    uint32
  TicksPerFrame uint32
  NumberFrame   uint32
  LOD*          blocks
```

Тайминги:

- `StartTime = FirstFrame * TicksPerFrame`;
- длительность = `(LastFrame - FirstFrame) * TicksPerFrame`;
- время ключей анимации — абсолютное, в тиках; при загрузке игра вычитает `StartTime`.

`NumberFrame` — число кадров, в которые рантайм пересэмплирует трансформацию ноды. Если в файле стоит `0`, игра вычисляет его как максимальное число `ANIMATION_MESH` у mesh-объектов.

`FrameSpeed` хранится, но в текущем загрузчике не используется.

### 6.3. `LOD` — уровень детализации

`Source/PluginMAX/Src/LodObject.cpp:3-70`.

```text
LOD:
  ID          uint32
  LOD         uint32   (номер уровня детализации)
  MATERIAL*   blocks
  OBJECT* / HELPER_OBJECT* / LIGHT_OBJECT* / CAMERA_OBJECT*
```

Порядок материалов и нод в файле не важен: reader принимает их в любом порядке.

### 6.4. `MATERIAL` — материал

`Source/PluginMAX/Src/MaterialObject.cpp:70-204`.

```text
MATERIAL:
  ID               uint32   (индекс в библиотеке материалов данного LOD)
  Name             string
  Shading          uint32   (1 = phong, иначе blinn)
  Parent           string   (для Multi/Sub-Object)
  NumberSubObject  uint32
  IntVar1          int32    (SelfIllumColorOn)
  IntVar2          int32    (Falloff)
  BlendType        int32    (0 = normal, 1 = add, 2 = sub)
  ANIMATION_AMBIENT*
  ANIMATION_DIFFUSE*
  ANIMATION_SPECULAR*
  ANIMATION_EMISSIVE*
  ANIMATION_FLOAT_VAR1*  (shininess)
  ANIMATION_FLOAT_VAR2*  (shininess strength)
  ANIMATION_FLOAT_VAR3*  (transparency)
  SUBTEXMAP*
```

Поведение в игре:

- `Shading == 1` — phong, иначе blinn;
- `Falloff != 0` — объект получает `ATTRUNKOBJ_COLLISIONTRACE` (skin);
- `BlendType`:
  - `0` — обычное смешивание,
  - `1` — additive,
  - `2` — subtractive;
- `Transparency` в рантайме превращается в alpha как `1 - value`;
- `Shininess` умножается на 100 при загрузке.

### 6.5. `SUBTEXMAP` — текстурная карта

`Source/PluginMAX/Src/AnimationMaterial.cpp:3-56`.

```text
SUBTEXMAP:
  ID            uint32   (слот текстуры)
  FileName      string   (имя файла текстуры)
  ChannelNumber uint32   (UV-канал)
  ANIMATION_MATRIX*      (опционально, анимация UV-трансформации)
```

`ID` — слот текстуры из `eTextureMapType` (`SceneMesh.h:30-44`):

| ID | Слот |
|---:|---|
| 0 | Ambient |
| 1 | Diffuse |
| 2 | Specular |
| 3 | Shininess |
| 4 | Shininess strength |
| 5 | Self-illumination |
| 6 | Opacity |
| 7 | Filter color |
| 8 | Bump |
| 9 | Reflection |
| 10 | Refraction |
| 11 | Displacement |

Игра реально использует только `DI` (diffuse), `OP` (opacity), `RL` (reflection), `BU` (bump). Имя текстуры берётся как basename и ищется в каталоге `textures/` рядом с моделью.

`ANIMATION_MATRIX` — ключи вида `time + 12 float` (матрица 3×4). Используется для анимации UV-развёртки.

### 6.6. Общие поля нод

`OBJECT`, `HELPER_OBJECT`, `LIGHT_OBJECT`, `CAMERA_OBJECT` наследуют `sNodeObject` (`SceneMesh.h:135-228`):

```text
ID       uint32
Name     string
Parent   string
Matrix   float12   (3×4, трансформация относительно родителя)
```

Далее идут общие анимационные блоки:

```text
ANIMATION_VISIBILITY*
ANIMATION_POSITION*
ANIMATION_ROTATION*
ANIMATION_SCALE*
ANIMATION_SCALEROTATION*
```

Важно: **текущий загрузчик игры не использует `Matrix` из файла**. Он собирает матрицу ноды из `ANIMATION_POSITION` / `ANIMATION_ROTATION` / `ANIMATION_SCALE` / `ANIMATION_SCALEROTATION` (`ObjLibrary.cpp:51-106`). Если анимационных ключей нет, матрица остаётся единичной. Поэтому при генерации M3D нужно заполнять именно animation-треки, а не только `Matrix`.

### 6.7. `OBJECT` — mesh-объект

`Source/PluginMAX/Src/ObjectMesh.cpp:3-69`.

```text
OBJECT:
  общие поля ноды
  ANIMATION_MESH*
```

### 6.8. `ANIMATION_MESH` — состояние геометрии

`Source/PluginMAX/Src/AnimationMesh.cpp:3-58`.

```text
ANIMATION_MESH:
  ID    uint32
  Time  uint32
  VERTEX_MESH
  FACE_MESH
  VERTEXNORMAL
  FACENORMAL
  CHANNEL_MAPPING*
```

Внутри:

```text
VERTEX_MESH:
  Number + N * float3   (x, y, z)

FACE_MESH:
  Number + N * ushort4  (i0, i1, i2, materialIndex)

VERTEXNORMAL:
  Number + N * float3   (nx, ny, nz)

FACENORMAL:
  Number + N * float3   (nx, ny, nz)
```

`Face[k][0..2]` — индексы вершин, `Face[k][3]` — индекс материала. Игра берёт материал **по первой грани** (`Face[0][3]`) и считает, что весь объект использует один материал (`ObjLibrary.cpp:251-255`). Если в модели несколько материалов на один объект, рантайм это не поддержит.

`VERTEXNORMAL` — нормали вершин. В рантайме требуется, чтобы их количество совпадало с количеством вершин (`VISASSERT(Vertex.size()==Normal.size())`). Если `VERTEXNORMAL` пуст, включается старый путь без нормалей.

`FACENORMAL` пишется/читается, но текущим загрузчиком **не используется**.

### 6.9. `CHANNEL_MAPPING` — UV-канал

`Source/PluginMAX/Src/ChannelMapping.cpp:3-45`.

```text
CHANNEL_MAPPING:
  ID     uint32   (0 = color vertex, 1 = основной UV, 2+ = дополнительные UV)
  Number uint32   (ChannelNumber)
  TEXVERTEX
  TEXFACE
```

```text
TEXVERTEX:
  Number + N * float2   (u, v)     // версии 2/3
  Number + N * float3   (u, v, w)  // версия 1

TEXFACE:
  Number + N * ushort3  (i0, i1, i2)  // версии 2/3
  Number + N * int3     (i0, i1, i2)  // версия 1
```

Игра использует **только первый** `CHANNEL_MAPPING` (`ChannelMappingLibrary[0]`) как UV-развёртку. Дополнительные UV-каналы игнорируются.

### 6.10. `HELPER_OBJECT` — Dummy/Group

`Source/PluginMAX/Src/HelperObject.cpp:3-79`.

```text
HELPER_OBJECT:
  общие поля ноды
  NumberSubObject uint32   (idObject)
  анимационные блоки
```

Дополнительно reader принимает поле `HelperName` со строкой `"Dummy"` или `"Group"` и по нему выставляет `idObject`. Writer пишет `NumberSubObject`.

`idObject` (`SceneMesh.h:46-59`):

| Значение | Тип |
|---:|---|
| 0 | Unknown |
| 1 | Dummy |
| 2 | Group |
| 3 | Omni light |
| 4 | Target spot |
| 5 | Directional |
| 6 | Free spot |
| 7 | Target directional |
| 8 | Free camera |
| 9 | Target camera |
| 10 | Parallel camera |

### 6.11. `LIGHT_OBJECT` — источник света

`Source/PluginMAX/Src/ObjectLight.cpp:3-85`.

```text
LIGHT_OBJECT:
  общие поля ноды
  NumberSubObject uint32   (idObject)
  Matrix1         float12  (OfsMatrix)
  IntVar1         int32    (UseGlobal)
  FileName        string   (TexProj, опционально)
  анимационные блоки
  ANIMATION_LIGHT*
```

`OfsMatrix` в текущем рантайме не используется. `UseGlobal != 0` даёт объекту `ATTRUNKOBJ_COLLISIONTRACE`. `TexProj` — проекционная текстура света.

`ANIMATION_LIGHT` (`Source/PluginMAX/Src/AnimLight.cpp:3-53`):

```text
ANIMATION_LIGHT:
  ID      uint32
  Time    uint32
  DiffuseColor float3   (r, g, b)
  FloatVar1    float    (TDist)
  FloatVar2    float    (FarBeginAttenuation)
  FloatVar3    float    (FarFinishAttenuation)
```

Игра использует `FarBeginAttenuation` как радиус света, `FarFinishAttenuation` — как дальность затухания.

### 6.12. `CAMERA_OBJECT` — камера

`Source/PluginMAX/Src/ObjectCamera.cpp:3-73`.

```text
CAMERA_OBJECT:
  общие поля ноды
  NumberSubObject uint32   (idObject)
  FileName        string   (TexProj, опционально)
  анимационные блоки
```

В текущем загрузчике игры камеры из M3D **пропускаются** (`ObjLibrary.cpp:887-905` обрабатывает только `MESH`, `LIGHT`, `HELPER`).

---

## 7. Форматы анимационных ключей

### 7.1. Трансформация ноды

| Блок | Элемент | Смысл |
|---|---|---|
| `ANIMATION_VISIBILITY` | `float2` | `time, visible` |
| `ANIMATION_POSITION` | `float4` | `time, x, y, z` |
| `ANIMATION_ROTATION` | `float5` | `time, x, y, z, w` |
| `ANIMATION_SCALE` | `float4` | `time, sx, sy, sz` |
| `ANIMATION_SCALEROTATION` | `float5` | `time, x, y, z, w` |

Все блоки — массивы вида `Number + N * element`.

При загрузке игра строит кватернион как `QuatF(-w, x, y, z)` (`ObjLibrary.cpp:72, 80, 94, 102`), то есть компонент `w` при преобразовании меняет знак. Это важно учитывать, если вы генерируете M3D вручную.

### 7.2. Материал

| Блок | Элемент | Смысл |
|---|---|---|
| `ANIMATION_AMBIENT` | `float4` | `time, r, g, b` |
| `ANIMATION_DIFFUSE` | `float4` | `time, r, g, b` |
| `ANIMATION_SPECULAR` | `float4` | `time, r, g, b` |
| `ANIMATION_EMISSIVE` | `float4` | `time, r, g, b` |
| `ANIMATION_FLOAT_VAR1` | `float2` | `time, value` — shininess |
| `ANIMATION_FLOAT_VAR2` | `float2` | `time, value` — shininess strength |
| `ANIMATION_FLOAT_VAR3` | `float2` | `time, value` — transparency |
| `ANIMATION_MATRIX` | `float13` | `time + 12 float` — UV-матрица 3×4 |

### 7.3. UV-анимация

`ANIMATION_MATRIX` — это массив `AniMatrix` (`meshfile.h:79-83`):

```cpp
struct AniMatrix {
    float time;
    MatXf mat;   // 12 float: 3×3 rotation + translation
};
```

То есть каждый ключ — 13 float: время и матрица 3×4.

---

## 8. Версии и совместимость

- **Версия 1**
  - ID полей и блоков — по 2 байта.
  - Грани: field type 20 (`int4`), текстурные грани: field type 28 (`int3`), текстурные вершины: field type 27 (`float3`).
  - Анимация материала могла храниться в старом блоке `ANIMATION_MATERIAL` (`0x14`), где ключи шли с полями `ID`, `Time`, `AmbientColor`, `DiffuseColor`, `SpecularColor`, `FloatVar1..3`, `IntVar1..2`, `SUBTEXMAP`. Этот путь обрабатывается `sMaterialObject::ReadOld` (`MaterialObject.cpp:3-69`).

- **Версии 2 и 3**
  - ID полей и блоков — по 1 байту.
  - Компактная геометрия: `Face` — `ushort4` (8 байт), `TexFace` — `ushort3` (6 байт), `TexVertex` — `float2` (8 байт).
  - Анимация материала — отдельными блоками `ANIMATION_AMBIENT`, `ANIMATION_DIFFUSE` и т.д.
  - Версии 2 и 3 в текущем парсере читаются одинаково; максимальная версия — 3.

- **Текстовый режим `'t'`**
  - Writer умеет писать текстовый вариант с именами полей (`Name`, `Vertex`, `$BLOCK` и т.д.).
  - Reader для текстовых полей фактически не реализован (`assert(0)`), поэтому игра использует только `'b'`.

---

## 9. Что реально использует игра

`cObjLibrary::LoadM3D` (`Source/Render/src/ObjLibrary.cpp:770-1014`) читает M3D так:

1. `f.ReadHeaderFile()`.
2. `cMeshScene::Read(f)` — корневой блок `SCENE`.
3. Берёт **первый канал** `MeshScene.ChannelLibrary[0]`.
4. Берёт **первый LOD** `LodLibrary[0]`; в `LoadM3D` стоит `VISASSERT(Channel->LodLibrary.length()==1)`. Дополнительные LOD внутри одного M3D формально поддерживаются, но этот загрузчик их не использует. Для дальнего LOD игра отдельно грузит файл с суффиксом `_lod` (`LoadLod`, `ObjLibrary.cpp:1016-1024`).
5. Для каждого объекта:
   - материал выбирается по `Face[0][3]`;
   - UV — первый `CHANNEL_MAPPING`;
   - нормали — `VERTEXNORMAL`, `FACENORMAL` игнорируется;
   - трансформация — из animation-треков, `Matrix` ноды игнорируется;
   - камеры пропускаются;
   - `OfsMatrix` света не используется;
   - `FrameSpeed` не используется.

Также:

- `Box[6]` в `sAnimationMesh` есть в структуре, но в файл не пишется.
- `Comment` есть в таблице полей, но объектные reader'ы его не обрабатывают.
- `LOGIC`-блок и `cLogicScene` в текущем рантайме не используются: `.l3d` грузится через тот же `cMeshScene` (`LogicGeneric.cpp:359-363`).

---

## 10. Эффекты, привязанные к нодам

В `Source/PluginMAX/m3d.txt` и `ObjLibrary.cpp:724-767` описано соглашение:

- Имя ноды: `effect: <effect_file> <effect_name>`.
- Для эффекта важны:
  1. положение и поворот ноды;
  2. флаг видимости.
- Если эффект зацикленный, `ANIMATION_VISIBILITY` включает/выключает генерацию частиц.
- Если эффект незацикленный, включение видимости запускает эффект.

Это не отдельный блок M3D, а интерпретация имени ноды и её animation-треков.

---

## Краткий итог

M3D — это:

- 9-байтовый заголовок `"m3d\0" + 'b'/'t' + int32 version`;
- поток полей `field_type + payload`, где `field_type == 0` открывает/закрывает блок;
- иерархия `SCENE → CHANNEL → LOD → MATERIAL/OBJECT/...`;
- геометрия в виде `VERTEX_MESH` (`float3`), `FACE_MESH` (`ushort4` с материалом), `VERTEXNORMAL` (`float3`), `CHANNEL_MAPPING` (`TEXVERTEX float2`, `TEXFACE ushort3`);
- анимация нод и материалов в виде массивов `Number + N * key`, где ключ — время плюс 2–13 float;
- little-endian, без выравнивания и без сжатия внутри самого M3D.

Если нужно, могу отдельно расписать минимальный парсер или структуру конкретного блока в виде C++-кода.

# Анимации в M3D

Вершинная (morph) анимация в M3D — лишь **один из нескольких** видов, причём в текущем рантайме игры она фактически не проигрывается.

## Что на самом деле анимируется в M3D

| Вид анимации | Блоки в файле | Где используется |
|---|---|---|
| Трансформация нод (позиция/поворот/масштаб) | `ANIMATION_POSITION`, `ANIMATION_ROTATION`, `ANIMATION_SCALE`, `ANIMATION_SCALEROTATION` | Основной механизм: `ObjLibrary.cpp:51-106`, `ReadAnimNode` `ObjLibrary.cpp:488-529` |
| Видимость нод | `ANIMATION_VISIBILITY` | `ObjLibrary.cpp:494-500` |
| Цвета и параметры материала | `ANIMATION_AMBIENT/DIFFUSE/SPECULAR/EMISSIVE`, `ANIMATION_FLOAT_VAR1..3` | `ReadMeshAnimMat` `ObjLibrary.cpp:355-470` |
| UV-трансформация текстуры | `SUBTEXMAP` → `ANIMATION_MATRIX` | `ObjLibrary.cpp:453-467` |
| Параметры источника света | `ANIMATION_LIGHT` | `ReadLightAnimMat` `ObjLibrary.cpp:471-486` |
| **Геометрия (вершины)** | `OBJECT` → `ANIMATION_MESH` (несколько состояний с разным `Time`) | Формально читается, но см. ниже |

То есть «вершинная» — только последняя строка.

## Почему вершинная анимация здесь не работает

В `Source/Render/src/ObjLibrary.cpp:108-139` (`ReadMeshBound` и `ReadMeshTri`) код ищет состояние по времени, но затем **всегда присваивает первое**:

```cpp
sAnimationMesh *AnimationMesh=0;
for(int nAnimMesh=0; nAnimMesh<ObjectMesh->AnimationMeshLibrary.length(); nAnimMesh++)
    if(ObjectMesh->AnimationMeshLibrary[nAnimMesh]->time<=time)
        AnimationMesh=ObjectMesh->AnimationMeshLibrary[0];   // <- всегда [0]
```

Похоже на баг: должно быть `[nAnimMesh]`. Из-за этого даже при наличии нескольких `ANIMATION_MESH` в M3D игра берёт только первое состояние геометрии, а `FACENORMAL` и `Box[6]` вообще не используются. Количество этих состояний влияет лишь на `NumberFrame`, если в `CHANNEL` он не задан (`ChannelAnimation.cpp:63-72`).

## Скелетной анимации (skinning) в формате нет

В M3D нет:

- костей как отдельного типа данных,
- весов привязки на вершину,
- bind pose / inverse bind matrices,
- индексных буферов скелета.

Иерархия нод (`Parent`, `OBJECT`/`HELPER_OBJECT`) даёт **жёсткую** привязку целых мешей к нодам: каждая нода имеет собственные TRS-треки, а меш движется целиком. Это «риггинг целыми объектами», а не деформация поверхности.

## Как игра на самом деле проигрывает анимацию

При загрузке исходные TRS-треки ноды сэмплируются в `Channel->NumberFrame` кадров и запекаются в массив матриц (`ObjLibrary.cpp:502-527`):

```cpp
AnimChannel->NewMatrix(Channel->ID, Channel->NumberFrame);
for(int nFrame=0; nFrame<Channel->NumberFrame; nFrame++) {
    MatXf Matrix;
    GetMatrixObj(StartTime+TimeFrame, Matrix, LodObject, NodeObject);
    AnimChain->GetMatrix(nFrame).time = TimeFrame;
    AnimChain->GetMatrix(nFrame).mat  = Matrix;
}
```

Дальше `cAnimChainNode::GetMatrix` (`Source/Render/src/AnimChannelNode.cpp:16-51`) просто линейно интерполирует эти запечённые матрицы по фазе. Если треков трансформации нет (или в них одна точка) — используется одна матрица, то есть нода статична.

## Итог

- Анимации в M3D **не только вершинные**: есть анимация трансформаций нод, видимости, материалов, UV-матриц и света.
- «Вершинная» анимация в формате предусмотрена (`ANIMATION_MESH` с разными `Time`), но текущий загрузчик её не переключает — всегда берёт первое состояние.
- Скелетной анимации со skinning в формате нет; роль скелета выполняет иерархия нод с жёсткой привязкой мешей.