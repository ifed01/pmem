#ifndef __ALLOCATOR_H
#define __ALLOCATOR_H
#include "intarith.h"

#include <assert.h>
#include <vector>
#include <algorithm>

typedef std::pair<uint64_t, size_t> interval_t;

class AllocatorLevel
{
protected:
  typedef uint64_t slot_t;
  
  // fitting into cache line on x86_64
  static const size_t slot_set_width = 8; // 8 slots per set
  static const size_t slot_set_bytes = sizeof(slot_t) * slot_set_width;
  static const slot_t all_set_slot_l0 = 0xffffffffffffffff;
  static const slot_t all_free_slot_l0 = 0;

  virtual uint64_t _children_per_slot() const = 0;
  virtual uint64_t _level_granularity() const = 0;

};
class AllocatorLevel01 : public AllocatorLevel
{
protected:
  std::vector<slot_t> l0;
  std::vector<slot_t> l1;
  uint64_t l0_granularity = 0; // space per entry
  uint64_t l1_granularity = 0; // space per entry

  uint64_t _level_granularity() const override
  {
    return l1_granularity;
  }
  void _init(uint64_t capacity, uint64_t _alloc_unit)
  {
    size_t elem_count = capacity / _alloc_unit / slot_set_bytes / 8;
    l0.resize(elem_count, 0);
    l0_granularity = _alloc_unit;
    elem_count = elem_count / _children_per_slot();
    l1.resize(elem_count, 0);
    // 512 bits at L0 mapped to L1 entry
    l1_granularity = l0_granularity * slot_set_bytes * 8;
  }
};

class AllocatorLevel01Loose : public AllocatorLevel01
{
  enum {
    L1_ENTRY_WIDTH = 2,
    L1_ENTRY_MASK = (1 << L1_ENTRY_WIDTH) - 1,
    L1_ENTRY_FREE = 0x00,
    L1_ENTRY_FULL = 0x01,
    L1_ENTRY_PARTIAL = 0x02,
    CHILD_PER_SLOT = sizeof(slot_t) * 8 / L1_ENTRY_WIDTH, // 32
    CHILD_PER_SLOT_L0 = sizeof(slot_t) * 8, // 64
  };
  uint64_t _children_per_slot() const override
  {
    return CHILD_PER_SLOT;
  }

  uint64_t _get_longest_from_l0(uint64_t pos0, uint64_t pos1,
    uint64_t* pos0_res, uint64_t* pos1_res) const
  {
    uint64_t res = 0;
    uint64_t res_candidate = 0;

    auto pos = pos0;
    uint64_t pos_free_start;
    bool was_free = false;
    auto d = slot_set_width * 8;
    slot_t bits = l0[pos / d];
    bits >>= pos % d;
    if (pos >= pos1) {
      return res;
    }
    bool end_loop;
    do {
      if ((bits & 1) == 0) {
        ++res_candidate;
        if (!was_free) {
          pos_free_start = pos;
          was_free = true;
        }
      } 
      end_loop = ++pos >= pos1;
      if (was_free && (end_loop || (bits & 1))) {
        res = std::max(res, res_candidate);
        *pos0_res = pos_free_start;
        *pos1_res = pos;
        was_free = false;
      }
      if ((pos % d) == 0) {
        bits = l0[pos / d];
      } else {
        bits >>= 1;
      }
    } while (!end_loop);
    res *= l0_granularity;
    return res;
  }

  bool _get_any_free_from_l0(uint64_t pos0, uint64_t pos1,
    uint64_t* res_pos0, uint64_t* res_pos1) const
  {
    auto pos = pos0;
    auto d = slot_set_width * 8;
    slot_t bits = l0[pos / d];
    bits >>= pos % d;
    while (pos < pos1) {
      if (bits & 1) {
        *res_pos0 = pos;
        *res_pos1 = pos + 1;
        return true;
      }
      ++pos;
      if ((pos % d) == 0) {
        bits = l0[pos / d];
      } else {
        bits >>= 1;
      }
    }
    return false;
  }

protected:
  struct search_ctx_t
  {
    size_t partial_count = 0;
    size_t free_count = 0;
    uint64_t free_l1_pos = 0;

    uint64_t max_len = 0;
    uint64_t max_l0_pos_start = 0;
    uint64_t max_l0_pos_end = 0;
    uint64_t min_affordable_len = 0;
    uint64_t affordable_l0_pos_start = 0;
    uint64_t affordable_l0_pos_end = 0;

    bool fully_processed = false;

    void reset()
    {
      *this = search_ctx_t();
    }
  };
  enum {
    NO_STOP,
    STOP_ON_EMPTY,
    STOP_ON_PARTIAL,
  };
  void _analyze_partials(uint64_t pos_start, uint64_t pos_end,
    uint64_t length, uint64_t min_length,
    int mode,
    search_ctx_t* ctx)
  {
    auto d = CHILD_PER_SLOT;
    assert((pos_start % d) == 0);
    assert((pos_end % d) == 0);

    uint64_t l0_w = slot_set_width * CHILD_PER_SLOT_L0;

    uint64_t l1_pos = pos_start;
    bool prev_pos_partial = false;
    for (auto pos = pos_start / d; pos < pos_end / d; ++pos) {
      slot_t slot_val = l1[pos];
      for (auto c = 0; c < CHILD_PER_SLOT; c++) {
        switch (slot_val & L1_ENTRY_MASK) {
        case L1_ENTRY_FREE:
          prev_pos_partial = false;
          if (!ctx->free_count) {
            ctx->free_l1_pos = l1_pos;
          }
          ++ctx->free_count;
          if (mode == STOP_ON_EMPTY) {
            return;
          }
          break;
        case L1_ENTRY_FULL:
          prev_pos_partial = false;
          break;
        case L1_ENTRY_PARTIAL:
          uint64_t l;
          uint64_t p0, p1;
          ++ctx->partial_count;

          if (!prev_pos_partial) {
            l = _get_longest_from_l0(l1_pos * l0_w, (l1_pos + 1) * l0_w, &p0, &p1);
            prev_pos_partial = true;
          } else {
            l = _get_longest_from_l0((l1_pos - 1) * l0_w, (l1_pos + 1) * l0_w, &p0, &p1);
          }
          if (l >= length) {
            if ((ctx->min_affordable_len == 0) || 
               ((ctx->min_affordable_len != 0) &&
                 (l - length < ctx->min_affordable_len - l))) {
              ctx->min_affordable_len = l;
              ctx->affordable_l0_pos_start = p0;
              ctx->affordable_l0_pos_end = p1;
            }
          }
          if (l > ctx->max_len) {
            ctx->max_len = l;
            ctx->max_l0_pos_start = p0;
            ctx->max_l0_pos_end = p1;
          }
          if (mode == STOP_ON_PARTIAL) {
            return;
          }
          break;
        }
        slot_val >>= L1_ENTRY_WIDTH;
        ++l1_pos;
      }
    }
    ctx->fully_processed = true;
  }

  void _mark_l1_on_l0(int64_t l0_pos_start, int64_t l0_pos_end)
  {
    if (l0_pos_start == l0_pos_end) {
      return;
    }
    int64_t l0_w = slot_set_width * CHILD_PER_SLOT_L0;
    int64_t l1_w = sizeof(slot_t) * CHILD_PER_SLOT;

    int64_t pos = p2align(l0_pos_start, l0_w);
    int64_t pos_end = p2roundup(l0_pos_end, l0_w);

    int64_t l1_pos = pos / l0_w;
    int64_t prev_l1_pos = l1_pos;

    bool was_free_only = true;
    bool was_partial = false;
    bool end_loop;
    do {
      auto& slot_val = l0[pos / CHILD_PER_SLOT_L0];
      pos += CHILD_PER_SLOT_L0;

      if (slot_val == all_set_slot_l0) {
        was_free_only = false;
      }
      else if (slot_val != 0) {
        was_partial = true;
        was_free_only = false;
        // no need to check the current slot set, it's partial
        pos = p2roundup(pos, l0_w);
      }

      end_loop = pos >= pos_end;
      l1_pos = pos / l0_w;
      if (l1_pos != prev_l1_pos || end_loop) {
        uint64_t shift = (prev_l1_pos % l1_w) * L1_ENTRY_WIDTH;
        l1[prev_l1_pos / l1_w] &= ~(L1_ENTRY_MASK << shift);
        if (was_partial) {
          l1[prev_l1_pos / l1_w] |= uint64_t(L1_ENTRY_PARTIAL) << shift;
        }
        else if (!was_free_only) {
          l1[prev_l1_pos / l1_w] |= uint64_t(L1_ENTRY_FULL) << shift;
        } else {
          l1[prev_l1_pos / l1_w] |= uint64_t(L1_ENTRY_FREE) << shift;
        }
        prev_l1_pos = l1_pos;
      }
    } while (!end_loop);
  }

  void _mark_alloc(int64_t l0_pos_start, int64_t l0_pos_end)
  {
    auto d0 = CHILD_PER_SLOT_L0;

    int64_t pos = l0_pos_start;
    slot_t bits = (slot_t)1 << (l0_pos_start % d0);

    while (pos < std::min(l0_pos_end, (int64_t)p2roundup(l0_pos_start, d0))) {
      l0[pos / d0] |= bits;
      bits <<= 1;
      pos++;
    }

    while (pos < std::min(l0_pos_end, (int64_t)p2align(l0_pos_end, d0))) {
      l0[pos / d0] = all_set_slot_l0;
      pos += d0;
    }
    bits = 1;
    while (pos < l0_pos_end) {
      l0[pos / d0] |= bits;
      bits <<= 1;
      pos++;
    }
    
    _mark_l1_on_l0(l0_pos_start, l0_pos_end);
  }
  void _mark_free(int64_t l0_pos_start, int64_t l0_pos_end)
  {
    auto d0 = CHILD_PER_SLOT_L0;

    auto pos = l0_pos_start;
    slot_t bits = (slot_t)1 << (l0_pos_start % d0);
    while (pos < std::min(l0_pos_end, (int64_t)p2roundup(l0_pos_start + 1, d0))) {
      l0[pos / d0] &= ~bits;
      bits <<= 1;
      pos++;
    }

    while (pos < std::min(l0_pos_end, (int64_t)p2align(l0_pos_end, d0))) {
      l0[pos / d0] = all_free_slot_l0;
      pos += d0;
    }
    bits = 1;
    while (pos < l0_pos_end) {
      l0[pos / d0] &= ~bits;
      bits <<= 1;
      pos++;
    }

    _mark_l1_on_l0(l0_pos_start, l0_pos_end);
  }

  interval_t _allocate_l1(uint64_t length, uint64_t min_length, uint64_t pos_start, uint64_t pos_end)
  {
    interval_t res = { 0, 0 };
    uint64_t l0_w = slot_set_width * CHILD_PER_SLOT_L0;

    if (length <= l0_granularity) {
      search_ctx_t ctx;
      _analyze_partials(pos_start, pos_end, l0_granularity, l0_granularity,
        STOP_ON_PARTIAL, &ctx);

      // check partially free slot sets first (including neighboring),
      // full length match required.
      if (ctx.min_affordable_len) {
        // allocate as specified
        assert(ctx.min_affordable_len >= length);
        auto pos_end = ctx.affordable_l0_pos_start + 1;
        _mark_alloc(ctx.affordable_l0_pos_start, pos_end);
        res = interval_t(ctx.affordable_l0_pos_start * l0_granularity, length);
        return res;
      }

      // allocate from free slot sets
      if (ctx.free_count) {
        _mark_alloc(ctx.free_l1_pos * l0_w, ctx.free_l1_pos * l0_w + 1);
        res = interval_t(ctx.free_l1_pos * l1_granularity, length);
        return res;
      }
    } else if (length == l1_granularity) {
      search_ctx_t ctx;
      _analyze_partials(pos_start, pos_end, length, min_length, STOP_ON_EMPTY, &ctx);
      
      // allocate exactly matched entry if any
      if (ctx.free_count) {
        _mark_alloc(ctx.free_l1_pos * l0_w, (ctx.free_l1_pos + 1) * l0_w);
        res = interval_t(ctx.free_l1_pos * l1_granularity, length);
        return res;
      }

      // we can terminate earlier on free entry only
      assert(ctx.fully_processed); 

      // check partially free slot sets first (including neighboring),
      // full length match required.
      if (ctx.min_affordable_len) {
        assert(ctx.min_affordable_len >= length);
        assert((length % l0_granularity) == 0);
        auto pos_end = ctx.affordable_l0_pos_start + length / l0_granularity;
        _mark_alloc(ctx.affordable_l0_pos_start, pos_end);
        res = interval_t(ctx.affordable_l0_pos_start * l0_granularity, length);
        return res;
      }
      if (ctx.max_len >= min_length) {
        assert((ctx.max_len % l0_granularity) == 0);
        auto pos_end = ctx.max_l0_pos_start + ctx.max_len / l0_granularity;
        _mark_alloc(ctx.max_l0_pos_start, pos_end);
        res = interval_t(ctx.max_l0_pos_start * l0_granularity, ctx.max_len);
        return res;
      }

      // if (length == l1_granularity)
    } else if (length < l1_granularity) {
      search_ctx_t ctx;
      _analyze_partials(pos_start, pos_end, length, min_length, NO_STOP, &ctx);
      assert(ctx.fully_processed);
      // check partially free slot sets first (including neighboring),
      // full length match required.
      if (ctx.min_affordable_len) {
        assert(ctx.min_affordable_len >= length);
        assert((length % l0_granularity) == 0);
        auto pos_end = ctx.affordable_l0_pos_start + length / l0_granularity;
        _mark_alloc(ctx.affordable_l0_pos_start, pos_end);
        res = interval_t(ctx.affordable_l0_pos_start * l0_granularity, length);
        return res;
      }
      // allocate exactly matched entry if any
      if (ctx.free_count) {

        assert((length % l0_granularity) == 0);
        auto pos_end = ctx.free_l1_pos * l0_w + length / l0_granularity;

        _mark_alloc(ctx.free_l1_pos * l0_w, pos_end);
        res = interval_t(ctx.free_l1_pos * l1_granularity, length);
        return res;
      }
      if (ctx.max_len >= min_length) {
        assert((ctx.max_len % l0_granularity) == 0);
        auto pos_end = ctx.max_l0_pos_start + ctx.max_len / l0_granularity;
        _mark_alloc(ctx.max_l0_pos_start, pos_end);
        res = interval_t(ctx.max_l0_pos_start * l0_granularity, ctx.max_len);
        return res;
      }
    } else {
      assert(false);
    }
    return res;
  }
  void _free_l1(const interval_t& r)
  {
    uint64_t l0_pos_start = r.first / l0_granularity;
    uint64_t l0_pos_end = round_up_to(r.first + r.second, l0_granularity) / l0_granularity;
    _mark_free(l0_pos_start, l0_pos_end);
  }
};

class AllocatorLevel01Compact : public AllocatorLevel01
{
  uint64_t _children_per_slot() const override
  {
    return 8;
  }
public:
};

#ifdef __NEVER_DEFINED
class PersistentAllocator
{
public:
  typedef std::pair<uint64_t, size_t> interval_t;

private:
  uint64_t min_alloc_unit = 0;
  uint64_t capacity = 0;
  uint64_t free_size = 0;
  
};



class PersistentAllocator0
{
public:
  typedef std::pair<uint64_t, size_t> interval_t;

private:
  uint64_t min_alloc_unit = 0;
  uint64_t capacity = 0;
  uint64_t free_size = 0;

  typedef uint64_t slot_t;

  enum {
    BOTTOM_BITS = 1, // per entry
    ROW0_BITS = 1, // per entry
    REGULAR_BITS = 2, // per entry
    ROWX_BITS = 2, // per entry
  };
  // entries per slot, all rows but 0th use 2(REGULAR) bits
  static const size_t slot_width_bottom = sizeof(slot_t) * 8 / BOTTOM_BITS;
  static const size_t slot_width = slot_width_bottom / REGULAR_BITS;
  static const slot_t all_slot_set = 0xffffffffffffffff;

  static const size_t slot_width_bits0 = slot_width * 8 / ROW0_BITS;
  static const size_t slot_width_bits = slot_width * 8 / ROWX_BITS;

  typedef std::vector<slot_t> row_t;
  typedef std::vector<row_t> bitmap_t;
  bitmap_t bm;
  std::vector<uint64_t> row_granularity;

  enum {
    DEFAULT,
  } cur_strategy = DEFAULT;
  enum {
    UNALLOCATED = 0,
    ALLOCATED_BUSY = 1,
    ALLOCATED_LOOSE = 2,
    ALLOCATED_NARROW = 3,
    ALLOCATED_MASK = 3,
  };

  size_t _choose_min_row(uint64_t min_alloc_size)
  {
    return 0; //FIXME!
  }
  inline uint64_t _mk_offs(size_t row, size_t pos0, size_t o);
  inline size_t _mk_next_pos(size_t row, size_t pos, size_t o)
  {
    auto _slot_width =
      row == 1 ? slot_width_bottom : slot_width;
  }
  size_t _mk_pos(size_t row, uint64_t offset) const
  {
    return 0; //FIXME
  }
  inline unsigned _get_row0_bits(size_t pos) const
  {
    return 0; //FIXME
  }
  inline void _set_row0_bits(size_t pos, unsigned val)
  {
  }

  void _get_side_intervals(size_t row, size_t pos, interval_t* left, interval_t* right);
  
  interval_t _choose_intervals(const interval_t& i1, const interval_t& i2,
    size_t length, size_t min_alloc_size)
  {
    if (i1.second == length) {
      return i1;
    } else if (i2.second == length) {
      return i2;
    } else if (i1.second > length && i2.second < length) {
      return i1;
    } else if (i2.second > length && i1.second < length) {
      return i2;
    } else if (i2.second > length && i1.second > length) {
      return i2.second - length > i1.second - length ? i1 : i2;
    } else if (i1.second < min_alloc_size && i2.second < min_alloc_size) {
      return interval_t(0, 0);
    }
    return i1.second > i2.second ? i1 : i2;
  }

  void _mark_rows(const interval_t& range, bool allocated)
  {
    
    size_t pos0 = _mk_pos(0, range.first);
    size_t pos1 = _mk_pos(0, range.first + range.second);

    size_t pos_start = p2align(pos0, slot_width_bits0);
    size_t pos_end = p2roundup(pos1, slot_width_bits0);
    size_t left_busy_pos = pos_start;
    size_t right_free_pos = pos_end;
    for (size_t pos = pos_start; pos < pos_end; ++pos)
    {
      if (pos < pos0 && (left_busy_pos + 1 == pos)) {
        if (_get_row0_bits(pos) == UNALLOCATED) {
          left_busy_pos = pos;
          right_free_pos = std::min(right_free_pos, pos);
        } else {
          right_free_pos = pos_end;
        }
      } else if (pos >= pos0 && pos1 < pos) {
        if (allocated) {
          _set_row0_bits(pos, ALLOCATED_BUSY);
          right_free_pos = pos_end;
        } else {
          _set_row0_bits(pos, UNALLOCATED);
          if (left_busy_pos + 1 == pos) {
            left_busy_pos = pos;
          }
          right_free_pos = std::min(right_free_pos, pos);
        }
      } else if (pos >= pos1) {
        if (_get_row0_bits(pos) == ALLOCATED_BUSY) {
          right_free_pos = pos_end;
        } else {
          if (left_busy_pos + 1 == pos) {
            left_busy_pos = pos;
          }
          right_free_pos = std::min(right_free_pos, pos);
        }
      }
    }
    auto next_level_flag =
      left_busy_pos == pos_end ? UNALLOCATED :
      left_busy_pos == pos_start && right_free_pos == pos_end ? ALLOCATED_BUSY :
      left_busy_pos > pos_start || right_free_pos < pos_end ? ALLOCATED_LOOSE :
      ALLOCATED_NARROW;

    for (auto i = 1; i < bm.size(); ++i) {
      size_t pos0 = _mk_pos(i, range.first);
      size_t pos1 = _mk_pos(i, range.first + range.second);
      
    }
  }

public:
  void init(uint64_t _capacity, uint64_t _alloc_unit)
  {
    assert(capacity == 0);
    assert(_capacity >= _alloc_unit);
    capacity = _capacity;
    min_alloc_unit = _alloc_unit;
    //FIXME: align capacity and min_alloc_unit

    size_t elem_count = capacity / min_alloc_unit;
    uint64_t granularity = min_alloc_unit;
    size_t _slot_width = slot_width_bottom;
    do {
      elem_count = round_up_to(elem_count, _slot_width) / _slot_width;
      bm.emplace_back(row_t(elem_count, 0));
      row_granularity.emplace_back(granularity);
      granularity *= elem_count;
      //FIXME: set excessive bits for each row

      // all rows but 0th use 2 bits per entry
      _slot_width = slot_width;

    } while (elem_count > 0);
    //FIXME: set excessive bits for top
  }
  interval_t allocate(size_t length, size_t min_alloc_size = 0)
  {
    std::pair<uint64_t, size_t> res = { 0, 0 };
    assert(capacity);
    assert(length >= min_alloc_size);
    if (!min_alloc_size) {
      min_alloc_size = length;
    }
    if (free_size < min_alloc_size) {
      return res;
    }

    uint64_t offset = 0;
    size_t row = bm.size() - 1;
    size_t min_row = _choose_min_row(min_alloc_size);

    res = _allocate_from_row(length, min_alloc_size, row, min_row, 0);
    return res;

  }
  void free(const interval_t& r)
  {
  }

  interval_t _allocate_from_row(size_t length, size_t min_alloc_size,
    size_t row, size_t min_row, size_t pos0)
  {
    interval_t res = { 0, 0 };
    assert(row >= min_row);
    if (row > min_row) {
      slot_t bits00 = bm[row][pos0];
      if (bits00 != all_slot_set) {
        slot_t bits0 = bits0;
        // choose the most occupied child
        for (int mask = ALLOCATED_NARROW; mask >= UNALLOCATED; mask--) {
          for (size_t o = 0; o < slot_width; o++) {
            auto bits = bits0 & ALLOCATED_MASK;
            if (bits == mask) {
              res = _allocate_from_row(length, min_alloc_size,
                row - 1, min_row, _mk_next_pos(row, pos0, o));
              if (res.second != 0) {
                //FIXME: mark everything
                return res;
              }
            }
            bits0 >>= REGULAR_BITS;
          }
        }
      }
    } else if (row == min_row && length <= row_granularity[row]) {
      //search for exact fit
      slot_t bits0 = bm[row][pos0];
      if (row == 0) {
        for (size_t o = 0; o < slot_width_bottom; o++) {
          if (bits0 && 1) {
            bm[row][pos0] &= (uint64_t)1 << o;
            res = std::make_pair(
              _mk_offs(row, pos0, o),
              std::max(length, row_granularity[row]));
            free_size -= row_granularity[row];
            return res;
          }
          bits0 >>= BOTTOM_BITS;
        }
      } else {
        for (size_t o = 0; o < slot_width; o++) {
          if ((bits0 & ALLOCATED_MASK) == UNALLOCATED) {
            bm[row][pos0] &= (ALLOCATED_BUSY) << (o * REGULAR_BITS);
            res = std::make_pair(_mk_offs(row, pos0, o), length);
            _mark_rows(res, true);
            return res;
          }
          bits0 >>= REGULAR_BITS;
        }
      }
    } 
    // row == min_row, length > row_granularity
    assert(row != 0); // this should be handled above

    interval_t next_interval = {0, 0};
    interval_t best_interval = {0, 0};

    slot_t bits0 = bm[row][pos0];
    for (size_t o = 0; o < slot_width; o++) {
      auto bits = bits0 & ALLOCATED_MASK;
      if (bits == ALLOCATED_BUSY || bits == ALLOCATED_NARROW) {
        if (next_interval.second != 0) {
          best_interval = _choose_intervals(best_interval,
            next_interval, length, min_alloc_size);
          next_interval.second = 0;
        }
        continue;
      } else if (bits == UNALLOCATED) {
        if (next_interval.second != 0) {
          next_interval.first = _mk_offs(row, pos0, o);
        }
        next_interval.second += row_granularity[row - 1];
      } else /*if (bits == ALLOCATED_LOOSE)*/ {
        interval_t left = { 0,0 };
        interval_t right = { 0,0 };
        _get_side_intervals(row - 1, _mk_next_pos(row, pos0, o), &left, &right);

        if (next_interval.second != 0) {
          if (left.second != 0) {
            next_interval.second += left.second;
          }
          best_interval = _choose_intervals(best_interval,
            next_interval, length, min_alloc_size);
        }
        next_interval = right;
      }
      bits0 >>= REGULAR_BITS;
    }
    if (next_interval.second != 0) {
      best_interval = _choose_intervals(best_interval,
        next_interval, length, min_alloc_size);
      next_interval.second = 0;
    }
    if (best_interval.second) {
      //....
      //FIXME: mark everything
      best_interval.second = std::min(best_interval.second, length);
      _mark_rows(best_interval, true);
      return best_interval;
    }
    return res;
  }
};
#endif

#endif
