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
struct VoxLayerRange
{
	uint16_t z0;
	uint16_t z1;
	uint8_t r;
	uint8_t g;
	uint8_t b;
	uint8_t pad;
};

class VoxColLayers
{
public:
	// Цвет (0x00RRGGBB) поверхности на высоте height: true, если height
	// попадает в диапазон колонки. Бинарный поиск колонки + поиск диапазона.
	bool lookup(uint32_t cellOffset, int height, uint32_t* rgb) const;

	// Покрасить [z0..z1] в (r,g,b): вычитание пересечений, вставка,
	// слияние смежных одноцветных. z1 >= z0 >= 0.
	void paint(uint32_t cellOffset, int z0, int z1,
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
	                      uint8_t b);
};

#endif // VOX_LAYERS_H
