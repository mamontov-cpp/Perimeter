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
#include <shared_mutex>
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
// Диапазон появился покраской ПУСТОТЫ (прозрачных вокселей) в сплошной
// цвет — типично покраска пола полости по срезу. Наследие цвета кепки
// такие диапазоны ПРОПУСКАЕТ: покраска пола не должна перекрашивать
// крышку острова (тело под полостью и материал у среза геометрически
// неотличимы — маркер ставит сама paint() по факту перекрытия
// прозрачного вокселя). Слияние диапазонов различает бит — мерж с
// немаркированным соседом запрещён, чтобы маркер не терялся.
static const uint8_t VOX_LAYER_OVER_VOID = 0x2;

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
			std::scoped_lock lock(accessMtx, o.accessMtx);
			col_ = o.col_;
			begin_ = o.begin_;
			end_ = o.end_;
			colHasTransparent_ = o.colHasTransparent_;
			ranges_ = o.ranges_;
			blockCount_ = o.blockCount_;
			blockLive_ = o.blockLive_;
			blockBegin_ = o.blockBegin_;
			liveRanges_ = o.liveRanges_;
			tombstones_ = o.tombstones_;
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

	// Resolve the heightfield's visible layer color under one lock. Returns
	// true when rgb was found; cutVoid suppresses a top decal on an unnamed
	// cavity floor. rawSurface/damHeight/viewSliceHeight are whole voxels.
	bool querySurfaceColor(uint32_t cellOffset, int rawSurface, int damHeight,
	                       int viewSliceHeight, uint32_t* rgb,
	                       bool* cutVoid) const;

	// Для кепок полостей (B5): под ОДНИМ локом отдаёт display height и
	// цвет/флаги верхнего вокселя (vxd-1) колонки по её ИНДЕКСУ — RowCaps
	// итерирует columnIndexRange и раньше звал lookup() +
	// cavityDisplayHeight(), каждый со своим findColumn по всей таблице.
	// Возвращает false, если колонки нет (тогда *displayH = -1).
	// topInherit (может быть nullptr): цвет ближайшего НЕпрозрачного
	// диапазона строго ниже верхнего вокселя — кепка красит «безымянный»/
	// прозрачный верх в цвет тела под ним, а не палитру поверхности
	// (inheritFallback, когда такого диапазона нет).
	// topFound (может быть nullptr): true, если верхний воксель попал в
	// КАКОЙ-ЛИБО диапазон — без этого флага сплошной диапазон (flags==0)
	// неотличим от «воксель безымянный», и покраска точно по верхнему
	// вокселю ([h-1..h-1]) на кепке пропадала.
	bool capInfo(int colIdx, int vxd, int* displayH, std::uint32_t* topRgb,
	             std::uint8_t* topFlags, std::uint32_t* topInherit = nullptr,
	             std::uint32_t inheritFallback = 0,
	             bool* topFound = nullptr) const;

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

	// Комбинированная нормализация колонки после изменения высоты (C2+C3):
	// ОДИН захват лока и ОДИН поиск колонки на всю операцию. Правила —
	// прежние (normalizeTransparentColumn): подъём (newHeight > oldHeight)
	// заливает прозрачные [0..newHeight) сплошными цветом rgb; верхний
	// воксель схлопывается к ближайшему сплошному под новым верхом;
	// prune режет диапазоны целиком над итоговой высотой. Возвращает
	// итоговую высоту дамбы (воксели). Слои переписываются ТОЛЬКО при
	// реальном изменении содержимого (C1: version не бампается на
	// идентичных данных — иначе оверлей получает ложный сигнал и делает
	// rebuild впустую). Быстрый выход (C3): у колонки нет прозрачных и
	// prune нечего резать — O(1) по кэшированному биту.
	// prunedOut (может быть nullptr): true, если prune вырезал хотя бы
	// один диапазон — враппер в игре по этому флагу сбрасывает true-color
	// краску поверхности (SurColBuf): срезанная под слои колонка должна
	// показывать ровный террейн (цвет зеропласта/палитры), а не старую
	// декаль.
	int normalize(uint32_t cellOffset, int oldHeight, int newHeight,
	              std::uint32_t rgb, bool prune, bool* prunedOut);

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
	// Same search without locking; caller must hold accessMtx and keep the
	// returned index valid for the duration of that lock.
	int findColumnImpl(uint32_t cellOffset) const;
	const VoxLayerRange* columnRanges(int colIdx, int* count) const;
	int columnCount() const { return static_cast<int>(col_.size()); }
	uint32_t columnOffset(int colIdx) const
	{
		std::shared_lock<std::shared_mutex> lock(accessMtx);
		return col_[static_cast<size_t>(colIdx)];
	}

	// Открытое хранилище: инвариант — col_ отсортирован; колонка i живёт в
	// ranges_[begin_[i], end_[i]) (begin_/end_ параллельны col_). Диапазоны
	// колонки отсортированы по z0 и не пересекаются. Между слотами и после
	// «хвоста» пула возможны ДЫРКИ (старые слоты после перезаписи колонки
	// в конец пула), а колонка с begin_==end_ — ТОМБСТОУН (удалённая: запись
	// остаётся, чтобы не делать memmove col_). Дырки и томбстоуны
	// упаковываются лениво compact() при превышении доли. Прямой доступ
	// нужен сериализации sidecar (VMAP.cpp) и снапшотам undo/diff редактора;
	// менять через paint()/setColumn().
	std::vector<uint32_t> col_;    // cell offsets, ascending
	std::vector<uint32_t> begin_;  // begin into ranges_ (parallel to col_)
	std::vector<uint32_t> end_;    // end into ranges_ (parallel to col_)
	std::vector<uint8_t> colHasTransparent_; // parallel bit: column has a
	                                         // transparent range (C3)
	std::vector<VoxLayerRange> ranges_; // per column sorted by z0, has gaps
	std::vector<uint32_t> blockCount_;  // mightHaveColumn: RECORDS per block
	std::vector<uint32_t> blockLive_;   // hasLiveColumn: live columns per block
	// Кэш «первая колонка блока»: blockBegin_[b] — индекс в col_ первой
	// колонки с offset >= b<<kBlockShift (+sentinel на конце). findColumn
	// ищет ВНУТРИ блока (единицы записей, горячий кэш) вместо бинарника по
	// всему col_ (192k записей, 768 KB, промахи кэша на каждом шаге).
	// Insertions and compact() rebuild blockBegin_ on the writer, so readers
	// never mutate index state under a shared lock.
	std::vector<uint32_t> blockBegin_;
	// Число живых диапазонов (сумма end_[i]-begin_[i]) и удалённых колонок —
	// триггеры ленивой упаковки в compact().
	uint64_t liveRanges_ = 0;
	uint64_t tombstones_ = 0;
	// Монотонный счётчик мутаций (setColumn): потребители (оверлей
	// полостей) скипают перестройки, пока слои и высоты не менялись —
	// игровые обновления видимости юнитов не трогают ни того, ни другого.
	uint64_t version = 0;
	// Отладка: offset последней мутировавшей колонки (кто и где правит
	// слои — см. voxperf.log).
	uint32_t lastMutationOff = 0;

	// Readers (render, mesher, color lookup) share accessMtx; terraforming
	// writers take it exclusively. Internal *Impl helpers and external raw
	// walks assume an already-held lock, so shared locks are never nested.
	mutable std::shared_mutex accessMtx;

	// Заменить данные колонки целиком (undo/diff). Пересоздаёт запись;
	// ranges == nullptr или count == 0 — удалить колонку (томбстоун).
	void setColumn(uint32_t cellOffset, const VoxLayerRange* ranges,
	               int count);

	// Упаковка пула: выбросить дырки и томбстоуны, пересобрать blockCount_.
	// O(N), вызывать редко — перед сохранением sidecar (игровой процесс
	// мир не пересохраняет; дырки/томбстоуны в рантайме дёшевы).
	void compact();

	bool empty() const
	{
		std::shared_lock<std::shared_mutex> lock(accessMtx);
		return liveRanges_ == 0;
	}
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
		std::shared_lock<std::shared_mutex> lock(accessMtx);
		const size_t b = static_cast<size_t>(cellOffset) >> kBlockShift;
		return b < blockCount_.size() && blockCount_[b] != 0;
	}
	// Игровой гейт обёртки tVoxSetAll: «у колонки ЕСТЬ живые слои».
	// blockCount_ считает ЗАПИСИ (томбстоуны тоже — иначе findColumn по
	// томбстоуну промахнётся и paint вставит дубликат), а здесь — только
	// живые колонки: срезанная под ноль колонка перестаёт звать normalize.
	bool hasLiveColumn(uint32_t cellOffset) const
	{
		std::shared_lock<std::shared_mutex> lock(accessMtx);
		const size_t b = static_cast<size_t>(cellOffset) >> kBlockShift;
		return b < blockLive_.size() && blockLive_[b] != 0;
	}
	// Построить индекс заново (загрузка sidecar пишет col_ напрямую).
	void buildBlockIndex();

private:
	static bool sameColor(const VoxLayerRange& a, uint8_t r, uint8_t g,
	                      uint8_t b, bool transparent);
	// Докинуть размер индекса блоков под offset (вставка колонки).
	void growBlockIndex(uint32_t cellOffset);
	// Rebuild blockBegin_ after insertion (unique lock already held).
	void rebuildBlockBegin();
	void buildBlockIndexImpl();
	void setColumnImpl(uint32_t cellOffset, const VoxLayerRange* ranges,
	                   int count);
};

#endif // VOX_LAYERS_H
