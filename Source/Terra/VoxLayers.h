// ============================================================================
// VoxLayers.h — внутренние цветовые слои вокселей (per-column ranges).
//
// Каждая колонка карты может нести набор ВЫСОТНЫХ ДИАПАЗОНОВ с цветом RGB24:
// диапазон [z0..z1] (абсолютные воксели от 0) применяется, когда поверхность
// колонки находится ВНУТРИ него. Покраска верха колонки задаёт «цвет по
// умолчанию» (true-color слой SurColBuf); слой побеждает, только пока
// поверхность внутри диапазона. Диапазоны непересекающиеся, отсортированы
// по z0; покраска ЗАМЕНЯЕТ цвет (вычитание пересечений + слияние смежных
// одноцветных). Диапазоны привязаны к абсолютной высоте → переживают
// терраформинг автоматически. Хранение разреженное: отсортированный массив
// индексов колонок (бинарный поиск) + общий пул диапазонов (внутри колонки
// — линейный/бинарный поиск, колонок мало — линия достаточна).
//
// Сериализация — секция sidecar heights16.bin (флаг LAYERS), см. VMAP.cpp.
// ============================================================================

#ifndef VOX_LAYERS_H
#define VOX_LAYERS_H

#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

// Один цветовой диапазон колонки. 8 байт (выравнивание).
// flags: бит 0 — прозрачные воксели ([z0..z1] ведут себя как отсутствие
// вокселя: цвет не рендерится, в терраформинг не участвуют, сквозь них
// видны другие воксели). Прозрачный диапазон не может быть верхним в
// колонке — инвариант поддерживается vrtMap::normalizeTransparentColumn
// (в game и редакторе). Байт pad раньше всегда писался нулём — старые
// sidecar читаются как «всё непрозрачное».
struct VoxLayerRange
{
	uint16_t z0;
	uint16_t z1;
	uint8_t r;
	uint8_t g;
	uint8_t b;
	uint8_t flags; // бит 0: VOX_LAYER_TRANSPARENT
};

static const uint8_t VOX_LAYER_TRANSPARENT = 0x1;

class VoxColLayers
{
public:
	// Копирование/перемещение целиком нужны редактору (swap базы и
	// эффективных слоёв в diff-режиме, снапшоты) — см. EditorSession.
	// mutex удаляет неявные операции, поэтому определяем вручную: лок на
	// источник и приёмник (редактор однопоточен на этих путях, гонки нет),
	// перемещение = копирование (слоёв немного — копия дешевле
	// усложнения контракта).
	VoxColLayers() = default;
	VoxColLayers(const VoxColLayers& o) { *this = o; }
	VoxColLayers& operator=(const VoxColLayers& o)
	{
		if (this != &o) {
			std::lock_guard<std::recursive_mutex> srcLock(o.accessMtx);
			std::lock_guard<std::recursive_mutex> dstLock(accessMtx);
			col_ = o.col_;
			begin_ = o.begin_;
			ranges_ = o.ranges_;
			blockCount_ = o.blockCount_;
			version = o.version;
			lastMutationOff = o.lastMutationOff;
		}
		return *this;
	}
	VoxColLayers(VoxColLayers&& o) { *this = o; }
	VoxColLayers& operator=(VoxColLayers&& o) { return *this = o; }

	// Цвет (0x00RRGGBB) поверхности на высоте height: true, если height
	// попадает в диапазон колонки. Бинарный поиск колонки + поиск диапазона.
	bool lookup(uint32_t cellOffset, int height, uint32_t* rgb) const;
	// Тот же поиск, но дополнительно сообщает флаги диапазона (бит 0 —
	// прозрачные воксели). Для прозрачного диапазона rgb не используется.
	bool lookup(uint32_t cellOffset, int height, uint32_t* rgb,
	            uint8_t* flags) const;
	// true, если на высоте height лежит прозрачный воксель.
	bool isTransparent(uint32_t cellOffset, int height) const;
	// Верхний СПЛОШНОЙ воксель строго ниже height (индекс z): проход
	// сквозь прозрачные диапазоны вниз, остановка на первом сплошном
	// (безымянные зазоры между диапазонами — сплошная порода); -1, если
	// сплошного нет. Один лок на весь проход — нормализация после
	// терраформинга ходит тут в цикле по вокселям.
	int topSolidVoxel(uint32_t cellOffset, int height) const;
	// Верхний непрозрачный воксель на высоте ≤ height: проходит сквозь
	// прозрачные диапазоны вниз. true + цвет, если такой есть.
	bool lookupSolidBelow(uint32_t cellOffset, int height, uint32_t* rgb) const;

	// УДАЛЕНИЕ «висячих» диапазонов: все диапазоны, целиком лежащие на
	// высоте >= height (над поверхностью колонки), вырезаются; колонка без
	// оставшихся диапазонов удаляется. Игровая гигиена: после того как
	// терраформинг срезал колонку под слои, они больше не нужны (поверхность
	// ниже них не вернётся — подъём сквозь пустоту заменяет их сплошными) и
	// только заставляют оверлей полостей считать колонку cavity-колонкой,
	// нормализацию ходить по ней снова и again. Редактор НЕ вызывает
	// (флаг vrtMap::pruneVoxRangesAboveSurface): там слои над поверхностью
	// сохраняются для редактирования.
	void pruneAbove(uint32_t cellOffset, int height);

	// Покрасить [z0..z1] в (r,g,b): вычитание пересечений, вставка,
	// слияние смежных одноцветных. z1 >= z0 >= 0. transparent помечает
	// диапазон как прозрачные воксели (цвет игнорируется при рендере).
	void paint(uint32_t cellOffset, int z0, int z1,
	           uint8_t r, uint8_t g, uint8_t b, bool transparent = false);

	// Заменить ПРОЗРАЧНЫЕ воксели диапазона [z0..z1] на сплошные с цветом
	// (r,g,b) — поднятие террейна сквозь прозрачные воксели. Непрозрачные
	// диапазоны не трогает.
	void solidify(uint32_t cellOffset, int z0, int z1,
	              uint8_t r, uint8_t g, uint8_t b);

	// Сырые данные колонки (для undo/diff/сериализации). colIdx — индекс
	// в col_/begin_ (-1 = колонки нет).
	int findColumn(uint32_t cellOffset) const;
	const VoxLayerRange* columnRanges(int colIdx, int* count) const;
	int columnCount() const { return static_cast<int>(col_.size()); }
	uint32_t columnOffset(int colIdx) const
	{
		std::lock_guard<std::recursive_mutex> lock(accessMtx);
		return col_[static_cast<size_t>(colIdx)];
	}

	// Открытое хранилище: инвариант — col_ отсортирован, begin_[i] —
	// начало диапазонов колонки i в ranges_, диапазоны колонки отсортированы
	// по z0 и не пересекаются. Прямой доступ нужен сериализации sidecar
	// (VMAP.cpp) и снапшотам undo/diff редактора; менять через paint().
	std::vector<uint32_t> col_;    // cell offsets, ascending
	std::vector<uint32_t> begin_;  // begin into ranges_; end = next begin
	std::vector<VoxLayerRange> ranges_; // per column sorted by z0
	std::vector<uint32_t> blockCount_;  // mightHaveColumn index, see above
	// Монотонный счётчик мутаций (setColumn): потребители (оверлей
	// полостей) скипают перестройки, пока слои и высоты не менялись —
	// игровые обновления видимости юнитов не трогают ни того, ни другого.
	uint64_t version = 0;
	// Отладка: offset последней мутировавшей колонки (кто и где правит
	// слои — см. voxperf.log).
	uint32_t lastMutationOff = 0;

	// Потоковая защита структуры. Логический поток игры мутирует колонки
	// во время терраформинга (normalizeTransparentColumn -> solidify ->
	// setColumn: erase/insert с реаллокацией векторов), графический поток
	// в это же время ходит по тем же векторам (getColor32,
	// TerraInterface::GetZ/GetZf, мешер и хэш оверлея полостей) — без
	// блокировки перестройка с логики даёт subscript-out-of-range на
	// рендере. recursive: методы вызывают друг друга (isTransparent ->
	// lookup, paint/solidify -> findColumn/columnRanges/setColumn), а
	// внешние проходы с сырым columnRanges держат лок на всё тело
	// (voxCavityAppendColumn, columnInputHash). Проходы, гонящиеся только
	// с самим собой (редактор, MT выключен), платят один неконтендед-лок.
	mutable std::recursive_mutex accessMtx;

	// Заменить данные колонки целиком (undo/diff). Пересоздаёт запись;
	// ranges == nullptr или count == 0 — удалить колонку.
	void setColumn(uint32_t cellOffset, const VoxLayerRange* ranges,
	               int count);

	bool empty() const { return col_.empty(); }
	void clear();

	// Индексный диапазон колонок с offsets в [offBeg..offEnd] (sorted col_ —
	// окно через lower_bound/upper_bound). Для оверлейного мешера полостей
	// и прочих проходов по подмножеству колонок.
	std::pair<int, int> columnIndexRange(uint32_t offBeg,
	                                     uint32_t offEnd) const;
	// true, если у колонки есть хотя бы один прозрачный диапазон
	// (мешер полостей: владение гранью на границе двух cavity-колонок).
	bool columnHasTransparent(uint32_t cellOffset) const;

	// Грубый блочный индекс по offset-ам: blockCount_[off >> kBlockShift]
	// — число колонок в блоке 1024 клетки. O(1)-предпроверка для горячих
	// путей (getColor32 на тексель тайловой текстуры при терраформинге —
	// двоичный поиск «колонки нет» стоил дороже самой раскраски).
	static const uint32_t kBlockShift = 10; // 1024 клетки на блок
	bool mightHaveColumn(uint32_t cellOffset) const
	{
		std::lock_guard<std::recursive_mutex> lock(accessMtx);
		const size_t b = static_cast<size_t>(cellOffset) >> kBlockShift;
		return b < blockCount_.size() && blockCount_[b] != 0;
	}
	// Построить индекс заново (загрузка sidecar пишет col_ напрямую).
	void buildBlockIndex();

private:
	static bool sameColor(const VoxLayerRange& a, uint8_t r, uint8_t g,
	                      uint8_t b, bool transparent);
	// Докинуть размер индекса блоков под offset (вставка колонки).
	void growBlockIndex(uint32_t cellOffset);
};

#endif // VOX_LAYERS_H
