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
	// Цвет (0x00RRGGBB) поверхности на высоте height: true, если height
	// попадает в диапазон колонки. Бинарный поиск колонки + поиск диапазона.
	bool lookup(uint32_t cellOffset, int height, uint32_t* rgb) const;
	// Тот же поиск, но дополнительно сообщает флаги диапазона (бит 0 —
	// прозрачные воксели). Для прозрачного диапазона rgb не используется.
	bool lookup(uint32_t cellOffset, int height, uint32_t* rgb,
	            uint8_t* flags) const;
	// true, если на высоте height лежит прозрачный воксель.
	bool isTransparent(uint32_t cellOffset, int height) const;
	// Верхний непрозрачный воксель на высоте ≤ height: проходит сквозь
	// прозрачные диапазоны вниз. true + цвет, если такой есть.
	bool lookupSolidBelow(uint32_t cellOffset, int height, uint32_t* rgb) const;

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
	uint32_t columnOffset(int colIdx) const { return col_[colIdx]; }

	// Открытое хранилище: инвариант — col_ отсортирован, begin_[i] —
	// начало диапазонов колонки i в ranges_, диапазоны колонки отсортированы
	// по z0 и не пересекаются. Прямой доступ нужен сериализации sidecar
	// (VMAP.cpp) и снапшотам undo/diff редактора; менять через paint().
	std::vector<uint32_t> col_;    // cell offsets, ascending
	std::vector<uint32_t> begin_;  // begin into ranges_; end = next begin
	std::vector<VoxLayerRange> ranges_; // per column sorted by z0

	// Заменить данные колонки целиком (undo/diff). Пересоздаёт запись;
	// ranges == nullptr или count == 0 — удалить колонку.
	void setColumn(uint32_t cellOffset, const VoxLayerRange* ranges,
	               int count);

	bool empty() const { return col_.empty(); }
	void clear();

private:
	static bool sameColor(const VoxLayerRange& a, uint8_t r, uint8_t g,
	                      uint8_t b, bool transparent);
};

#endif // VOX_LAYERS_H
