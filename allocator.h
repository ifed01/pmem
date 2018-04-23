#ifndef __ALLOCATOR_H
#define __ALLOCATOR_H
#include "intarith.h"

#include <assert.h>
#include <vector>
#include <algorithm>
#include <mutex>

typedef std::pair<uint64_t, size_t> interval_t;
typedef std::vector<interval_t> interval_list_t;
typedef uint64_t slot_t;

// fitting into cache line on x86_64
static const size_t slotset_width = 8; // 8 slots per set
static const size_t slotset_bytes = sizeof(slot_t) * slotset_width;
static const size_t bits_per_slot = sizeof(slot_t) * 8;
static const size_t bits_per_slotset = slotset_bytes * 8;
static const slot_t all_slot_set = 0xffffffffffffffff;
static const slot_t all_slot_clear = 0;

inline int find_next_set_bit(slot_t slot_val, int start_pos)
{
#ifdef __GNUC__
  if (start_pos == 0) {
    start_pos = __builtin_ffsll(slot_val);
    return start_pos ? start_pos - 1 : bits_per_slot;
  }
#endif
  slot_t mask = slot_t(1) << start_pos;
  while (start_pos < bits_per_slot && !(slot_val & mask)) {
    mask <<= 1;
    ++start_pos;
  }
  return start_pos;
}


class AllocatorLevel
{
protected:

  virtual uint64_t _children_per_slot() const = 0;
  virtual uint64_t _level_granularity() const = 0;

};

class AllocatorLevel01 : public AllocatorLevel
{
protected:
  std::vector<slot_t> l0; // set bit means free entry
  std::vector<slot_t> l1;
  uint64_t l0_granularity = 0; // space per entry
  uint64_t l1_granularity = 0; // space per entry

  uint64_t _level_granularity() const override
  {
    return l1_granularity;
  }
  void _init(uint64_t capacity, uint64_t _alloc_unit)
  {
    size_t elem_count = capacity / _alloc_unit / sizeof(slot_t) / 8;
    l0.resize(elem_count, all_slot_set);
    l0_granularity = _alloc_unit;
    elem_count = elem_count / _children_per_slot();
    l1.resize(elem_count, all_slot_set);
    // 512 bits at L0 mapped to L1 entry
    l1_granularity = l0_granularity * bits_per_slotset;
  }
  inline bool _is_slot_fully_allocated(uint64_t idx) const {
    return l1[idx] == all_slot_clear;
  }
};

template <class T>
class AllocatorLevel02;

class AllocatorLevel01Loose : public AllocatorLevel01
{
  enum {
    L1_ENTRY_WIDTH = 2,
    L1_ENTRY_MASK = (1 << L1_ENTRY_WIDTH) - 1,
    L1_ENTRY_FULL = 0x00,
    L1_ENTRY_PARTIAL = 0x01,
    L1_ENTRY_FREE = 0x03,
    CHILD_PER_SLOT = bits_per_slot / L1_ENTRY_WIDTH, // 32
    CHILD_PER_SLOT_L0 = bits_per_slot, // 64
  };
  uint64_t _children_per_slot() const override
  {
    return CHILD_PER_SLOT;
  }

  uint64_t _get_longest_from_l0(uint64_t pos0, uint64_t pos1,
    uint64_t* pos0_res, uint64_t* pos1_res) const
  {
    uint64_t res = 0;
    if (pos0 >= pos1) {
      return res;
    }

    uint64_t res_candidate = 0;

    auto pos = pos0;
    uint64_t pos_free_start;
    bool was_free = false;
    auto d = slotset_width * 8;
    slot_t bits = l0[pos / d];
    bits >>= pos % d;
    bool end_loop = false;
    do {
      if ((pos % d) == 0) {
        bits = l0[pos / d];
        if (pos1 - pos >= d) {
          if (bits == all_slot_set) {
            // slot is totally free
            if (was_free) {
              res_candidate += d;
            } else {
              was_free = true;
              res_candidate = d;
              pos_free_start = pos;
            }
            pos += d;
            end_loop = pos >= pos1;
            if (end_loop && res < res_candidate) {
              *pos0_res = pos_free_start;
              *pos1_res = pos;
              res = res_candidate;
              res_candidate = 0;
            }
            continue;
          }
          if (bits == all_slot_clear) {
            // slot is totally allocated
            if (was_free) {
              if (res < res_candidate) {
                *pos0_res = pos_free_start;
                *pos1_res = pos;
                res = res_candidate;
                res_candidate = 0;
              }
              was_free = false;
            }
            pos += d;
            end_loop = pos >= pos1;
            continue;
          }
        }
      } //if ((pos % d) == 0)

      if ((bits & 1) == 1) {
        // item is free
        ++res_candidate;
        if (!was_free) {
          pos_free_start = pos;
          was_free = true;
        }
      } 
      end_loop = ++pos >= pos1;
      if (was_free && (end_loop || !(bits & 1))) {
        if (res < res_candidate) {
          *pos0_res = pos_free_start;
          *pos1_res = pos;
          res = res_candidate;
          res_candidate = 0;
        }
        was_free = false;
      }
      bits >>= 1;
    } while (!end_loop);
    res *= l0_granularity;
    return res;
  }
  
  bool _allocate_l0(uint64_t length, uint64_t min_length,
    uint64_t l0_pos0, uint64_t l0_pos1,
    uint64_t* allocated,
    interval_list_t* res)
  {
    uint64_t d0 = CHILD_PER_SLOT_L0;

    assert(l0_pos0 < l0_pos1);
    assert(length > *allocated);
    assert(0 == (l0_pos0 % (slotset_width * d0)));
    assert(0 == (l0_pos1 % (slotset_width * d0)));

    uint64_t need_entries = (length - *allocated) / l0_granularity;

    for (auto idx = l0_pos0 / d0; (idx < l0_pos1 / d0) && (length > *allocated);
      ++idx) {
      slot_t& slot_val = l0[idx];
      auto base = idx * d0;
      if (slot_val == all_slot_clear) {
        continue;
      } else if (slot_val == all_slot_set) {
        uint64_t to_alloc = std::min(need_entries, d0);
        *allocated += to_alloc * l0_granularity;
        res->emplace_back(
          interval_t(base * l0_granularity, to_alloc * l0_granularity));
        if (to_alloc == d0) {
          slot_val = all_slot_clear;
        } else {
          _mark_alloc_l0(base, base + to_alloc);
        }
        need_entries -= d0;
        continue;
      }

      int free_pos = find_next_set_bit(slot_val, 0);
      assert(free_pos >= 0 && free_pos < bits_per_slot);
      auto next_pos = free_pos + 1;
      while (next_pos < bits_per_slot &&
        (next_pos - free_pos) < need_entries) {

        if (0 == (slot_val & (slot_t(1) << next_pos))) {
          auto to_allocate = (next_pos - free_pos) * l0_granularity;
          *allocated += to_allocate;
          res->emplace_back((base + free_pos) * l0_granularity,
            to_allocate);
          _mark_alloc_l0(base + free_pos, base + next_pos);
          need_entries -= (next_pos - free_pos);
          free_pos = find_next_set_bit(slot_val, next_pos + 1);
          next_pos = free_pos + 1;
        } else {
          ++next_pos;
        }
      }
      if (need_entries && free_pos < bits_per_slot) {
        uint64_t to_alloc = std::min(need_entries, d0 - free_pos);
        *allocated += to_alloc * l0_granularity;
        res->emplace_back((base + free_pos) * l0_granularity,
            to_alloc * l0_granularity);
        _mark_alloc_l0(base + free_pos, base + free_pos + to_alloc);
      }
    }
    return _is_empty_l0(l0_pos0, l0_pos1);
  }

  /*bool _get_any_free_from_l0(uint64_t pos0, uint64_t pos1,
    uint64_t* res_pos0, uint64_t* res_pos1) const
  {
    auto pos = pos0;
    auto d = slotset_width * 8;
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
  }*/

protected:

  friend class AllocatorLevel02<AllocatorLevel01Loose>;

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

    uint64_t l0_w = slotset_width * CHILD_PER_SLOT_L0;

    uint64_t l1_pos = pos_start;
    bool prev_pos_partial = false;
    for (auto pos = pos_start / d; pos < pos_end / d; ++pos) {
      slot_t slot_val = l1[pos];
      // FIXME minor: code below can be optimized to check slot_val against
      // all_slot_set(_clear)
      for (auto c = 0; c < CHILD_PER_SLOT; c++) {
        switch (slot_val & L1_ENTRY_MASK) {
        case L1_ENTRY_FREE:
          /*if (prev_pos_partial) {
            uint64_t l;
            uint64_t p0, p1;
            l = _get_longest_from_l0((l1_pos - 1) * l0_w, l1_pos * l0_w, &p0, &p1);
            l += l0_w * l0_granularity;
            p1 += l0_w;
            if (l >= length) {
              if ((ctx->min_affordable_len == 0) ||
                ((ctx->min_affordable_len != 0) &&
                (l - length < ctx->min_affordable_len - length))) {
                ctx->min_affordable_len = l;
                ctx->affordable_l0_pos_start = p0;
                ctx->affordable_l0_pos_end = p1;
              }
            }
          }*/
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
                 (l - length < ctx->min_affordable_len - length))) {
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


  void _mark_l1_on_l0(int64_t l0_pos, int64_t l0_pos_end)
  {
    if (l0_pos == l0_pos_end) {
      return;
    }
    auto d0 = bits_per_slotset;
    //auto d1 = bits_per_slotset * L1_ENTRY_WIDTH;
    uint64_t l1_w = CHILD_PER_SLOT;
    // this should be aligned with slotset boundaries
    assert(0 == (l0_pos % d0));
    assert(0 == (l0_pos_end % d0));

    int64_t idx = l0_pos / bits_per_slot;
    int64_t idx_end = l0_pos_end / bits_per_slot;
    bool was_all_free = true;
    bool was_all_allocated = true;
    
    auto l1_pos = l0_pos / d0;

    while (idx < idx_end) {
      if (l0[idx] == all_slot_clear) {
        was_all_free = false;
        
        // if not all prev slots are allocated then no need to check the
        // current slot set, it's partial
        ++idx;
        idx = 
          was_all_allocated ? idx : p2roundup(idx, int64_t(slotset_width));
      } else if (l0[idx] == all_slot_set) {
        // all free
        was_all_allocated = false;
        // if not all prev slots are free then no need to check the
        // current slot set, it's partial
        ++idx;
        idx = was_all_free ? idx : p2roundup(idx, int64_t(slotset_width));
      } else {
        // no need to check the current slot set, it's partial
        was_all_free = false;
        was_all_allocated = false;
        ++idx;
        idx = p2roundup(idx, int64_t(slotset_width));
      }
      if ((idx % slotset_width) == 0) {

        uint64_t shift = (l1_pos % l1_w) * L1_ENTRY_WIDTH;
        l1[l1_pos / l1_w] &= ~(uint64_t(L1_ENTRY_MASK) << shift);

        if (was_all_allocated) {
          assert(!was_all_free);
          l1[l1_pos / l1_w] |= uint64_t(L1_ENTRY_FULL) << shift;
        } else if (was_all_free) {
          assert(!was_all_allocated);
          l1[l1_pos / l1_w] |= uint64_t(L1_ENTRY_FREE) << shift;
        } else {
          l1[l1_pos / l1_w] |= uint64_t(L1_ENTRY_PARTIAL) << shift;
        }
        ++l1_pos;
      }
    }
  }

  void _mark_alloc_l0(int64_t l0_pos_start, int64_t l0_pos_end)
  {
    auto d0 = CHILD_PER_SLOT_L0;

    int64_t pos = l0_pos_start;
    slot_t bits = (slot_t)1 << (l0_pos_start % d0);

    while (pos < std::min(l0_pos_end, (int64_t)p2roundup(l0_pos_start, d0))) {
      l0[pos / d0] &= ~bits;
      bits <<= 1;
      pos++;
    }

    while (pos < std::min(l0_pos_end, (int64_t)p2align(l0_pos_end, d0))) {
      l0[pos / d0] = all_slot_clear;
      pos += d0;
    }
    bits = 1;
    while (pos < l0_pos_end) {
      l0[pos / d0] &= ~bits;
      bits <<= 1;
      pos++;
    }
  }
    
  void _mark_alloc_l1_l0(int64_t l0_pos_start, int64_t l0_pos_end)
  {
    _mark_alloc_l0(l0_pos_start, l0_pos_end);
    l0_pos_start = p2align(l0_pos_start, int64_t(bits_per_slotset));
    l0_pos_end = p2roundup(l0_pos_end, int64_t(bits_per_slotset));
    _mark_l1_on_l0(l0_pos_start, l0_pos_end);
  }

  void _mark_free_l0(int64_t l0_pos_start, int64_t l0_pos_end)
  {
    auto d0 = CHILD_PER_SLOT_L0;

    auto pos = l0_pos_start;
    slot_t bits = (slot_t)1 << (l0_pos_start % d0);
    while (pos < std::min(l0_pos_end, (int64_t)p2roundup(l0_pos_start + 1, d0))) {
      l0[pos / d0] |= bits;
      bits <<= 1;
      pos++;
    }

    while (pos < std::min(l0_pos_end, (int64_t)p2align(l0_pos_end, d0))) {
      l0[pos / d0] = all_slot_set;
      pos += d0;
    }
    bits = 1;
    while (pos < l0_pos_end) {
      l0[pos / d0] |= bits;
      bits <<= 1;
      pos++;
    }
  }

  void _mark_free_l1_l0(int64_t l0_pos_start, int64_t l0_pos_end)
  {
    _mark_free_l0(l0_pos_start, l0_pos_end);
    l0_pos_start = p2align(l0_pos_start, int64_t(bits_per_slotset));
    l0_pos_end = p2roundup(l0_pos_end, int64_t(bits_per_slotset));
    _mark_l1_on_l0(l0_pos_start, l0_pos_end);
  }

  bool _is_empty_l0(uint64_t l0_pos, uint64_t l0_pos_end)
  {
    bool no_free = true;
    uint64_t d = slotset_width * CHILD_PER_SLOT_L0;
    assert(0 == (l0_pos % d));
    assert(0 == (l0_pos_end % d));

    auto idx = l0_pos / CHILD_PER_SLOT_L0;
    auto idx_end = l0_pos_end / CHILD_PER_SLOT_L0;
    while (idx < idx_end && no_free) {
      no_free = l0[idx] == all_slot_clear;
      ++idx;
    }
    return no_free;
  }
  bool _is_empty_l1(uint64_t l1_pos, uint64_t l1_pos_end)
  {
    bool no_free = true;
    uint64_t d = slotset_width * _children_per_slot();
    assert(0 == (l1_pos % d));
    assert(0 == (l1_pos_end % d));

    auto idx = l1_pos / CHILD_PER_SLOT;
    auto idx_end = l1_pos_end / CHILD_PER_SLOT;
    while (idx < idx_end && no_free) {
      no_free = _is_slot_fully_allocated(idx);
      ++idx;
    }
    return no_free;
  }

  interval_t _allocate_l1(uint64_t length, uint64_t min_length, uint64_t pos_start, uint64_t pos_end)
  {
    interval_t res = { 0, 0 };
    uint64_t l0_w = slotset_width * CHILD_PER_SLOT_L0;

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
        _mark_alloc_l1_l0(ctx.affordable_l0_pos_start, pos_end);
        res = interval_t(ctx.affordable_l0_pos_start * l0_granularity, length);
        return res;
      }

      // allocate from free slot sets
      if (ctx.free_count) {
        _mark_alloc_l1_l0(ctx.free_l1_pos * l0_w, ctx.free_l1_pos * l0_w + 1);
        res = interval_t(ctx.free_l1_pos * l1_granularity, length);
        return res;
      }
    } else if (length == l1_granularity) {
      search_ctx_t ctx;
      _analyze_partials(pos_start, pos_end, length, min_length, STOP_ON_EMPTY, &ctx);
      
      // allocate exactly matched entry if any
      if (ctx.free_count) {
        _mark_alloc_l1_l0(ctx.free_l1_pos * l0_w, (ctx.free_l1_pos + 1) * l0_w);
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
        _mark_alloc_l1_l0(ctx.affordable_l0_pos_start, pos_end);
        res = interval_t(ctx.affordable_l0_pos_start * l0_granularity, length);
        return res;
      }
      if (ctx.max_len >= min_length) {
        assert((ctx.max_len % l0_granularity) == 0);
        auto pos_end = ctx.max_l0_pos_start + ctx.max_len / l0_granularity;
        _mark_alloc_l1_l0(ctx.max_l0_pos_start, pos_end);
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
        _mark_alloc_l1_l0(ctx.affordable_l0_pos_start, pos_end);
        res = interval_t(ctx.affordable_l0_pos_start * l0_granularity, length);
        return res;
      }
      // allocate exactly matched entry if any
      if (ctx.free_count) {

        assert((length % l0_granularity) == 0);
        auto pos_end = ctx.free_l1_pos * l0_w + length / l0_granularity;

        _mark_alloc_l1_l0(ctx.free_l1_pos * l0_w, pos_end);
        res = interval_t(ctx.free_l1_pos * l1_granularity, length);
        return res;
      }
      if (ctx.max_len >= min_length) {
        assert((ctx.max_len % l0_granularity) == 0);
        auto pos_end = ctx.max_l0_pos_start + ctx.max_len / l0_granularity;
        _mark_alloc_l1_l0(ctx.max_l0_pos_start, pos_end);
        res = interval_t(ctx.max_l0_pos_start * l0_granularity, ctx.max_len);
        return res;
      }
    } else {
      assert(false);
    }
    return res;
  }

  bool _allocate_l1(uint64_t length, uint64_t min_length,
    uint64_t l1_pos_start, uint64_t l1_pos_end,
    uint64_t* allocated,
    interval_list_t* res)
  {
    uint64_t d0 = CHILD_PER_SLOT_L0;
    uint64_t d1 = CHILD_PER_SLOT;

    assert(0 == (l1_pos_start % (slotset_width * d1)));
    assert(0 == (l1_pos_end % (slotset_width * d1)));
    if (min_length != l0_granularity) {
      // probably not the most effecient way but 
      // we don't care much about that at the moment
      bool has_space = true;
      while (length > *allocated && has_space) {
        interval_t i =
          _allocate_l1(length - *allocated, min_length, l1_pos_start, l1_pos_end);
        if (i.second == 0) {
          has_space = false;
        } else {
          res->push_back(i);
          *allocated += i.second;
        }
      }
    } else {
      uint64_t l0_w = slotset_width * d0;
      
      for (auto idx = l1_pos_start / d1;
        idx < l1_pos_end / d1 && length > *allocated;
        ++idx) {
        slot_t& slot_val = l1[idx];
        int free_pos = 0;
        if (slot_val == all_slot_clear) {
          continue;
        } else if (slot_val == all_slot_set) {
          uint64_t to_alloc = std::min(length - *allocated,
            l1_granularity * d1);
          *allocated += to_alloc;
          res->emplace_back(
            interval_t(idx * d1 * l1_granularity, to_alloc));

          _mark_alloc_l1_l0(idx * d1 * bits_per_slotset, idx * d1 * bits_per_slotset + to_alloc / l0_granularity);
          continue;
        }
        free_pos = find_next_set_bit(slot_val, free_pos);
        assert(free_pos >= 0 && free_pos < bits_per_slot);
        free_pos /= L1_ENTRY_WIDTH;
        do {
          assert(length > *allocated);

          bool empty;
          empty = _allocate_l0(length - *allocated, min_length,
            (idx * d1 + free_pos) * l0_w,
            (idx * d1 + free_pos + 1) * l0_w,
            allocated,
            res);
          slot_val &= ~(slot_t(L1_ENTRY_MASK) << (free_pos * L1_ENTRY_WIDTH));
          if (empty) {
            // the next line is no op with the current L1_ENTRY_FULL but left 
            // as-is for the sake of uniformity and to avoid potential errors
            // in future 
            slot_val |= slot_t(L1_ENTRY_FULL) << (free_pos * L1_ENTRY_WIDTH);
          } else {
            slot_val |= slot_t(L1_ENTRY_PARTIAL) << (free_pos * L1_ENTRY_WIDTH);
          }
          if (length <= *allocated || slot_val == all_slot_clear) {
            break;
          }
          ++free_pos;
          free_pos = find_next_set_bit(slot_val, free_pos) / L1_ENTRY_WIDTH;
        } while (free_pos <= d0);
      }
    }
    return _is_empty_l1(l1_pos_start, l1_pos_end);
  }

  void _free_l1(const interval_t& r)
  {
    uint64_t l0_pos_start = r.first / l0_granularity;
    uint64_t l0_pos_end = round_up_to(r.first + r.second, l0_granularity) / l0_granularity;
    _mark_free_l1_l0(l0_pos_start, l0_pos_end);
  }

public:
  uint64_t debug_get_allocated(uint64_t pos0 = 0, uint64_t pos1 = 0) const
  {
    auto d = CHILD_PER_SLOT;
    if (pos1 == 0) {
      pos1 = l1.size() * d;
    }
    auto avail = debug_get_free(pos0, pos1);
    return (pos1 - pos0) * l1_granularity - avail;
  }

  uint64_t debug_get_free(uint64_t l1_pos0 = 0, uint64_t l1_pos1 = 0) const
  {
    auto d0 = CHILD_PER_SLOT_L0;
    auto d1 = CHILD_PER_SLOT;
    assert(0 == (l1_pos0 % d1));
    assert(0 == (l1_pos1 % d1));

    auto idx0 = l1_pos0 * slotset_width;
    auto idx1 = l1_pos1 * slotset_width;

    if (idx1 == 0) {
      idx1 = l0.size();
    }

    uint64_t res = 0;
    for (uint64_t i = idx0; i < idx1; ++i) {
      auto v = l0[i];
      if (v == all_slot_set) {
        res += CHILD_PER_SLOT_L0;
      } else if (v != all_slot_clear) {
        size_t cnt = 0;
#ifdef __GNUC__
        cnt = __builtin_popcountll(v);
#else
        // Kernighan's Alg to count set bits
        while (v) {
          v &= (v - 1);
          cnt++;
        }
#endif
        res += cnt;
      }
    }
    return res * l0_granularity;
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

template <class L1>
class AllocatorLevel02 : public AllocatorLevel
{
public:
  uint64_t debug_get_free(uint64_t pos0 = 0, uint64_t pos1 = 0) const {
    return l1.debug_get_free(pos0 * l1._children_per_slot() * bits_per_slot,
      pos1 * l1._children_per_slot() * bits_per_slot);
  }
  uint64_t debug_get_allocated(uint64_t pos0 = 0, uint64_t pos1 = 0) const {
    return l1.debug_get_allocated(pos0 * l1._children_per_slot() * bits_per_slot,
      pos1 * l1._children_per_slot() * bits_per_slot);
  }

protected:
  std::mutex lock;
  L1 l1;
  std::vector<slot_t> l2;
  uint64_t l2_granularity = 0; // space per entry

  enum {
    CHILD_PER_SLOT = bits_per_slot, // 64
  };
  uint64_t _children_per_slot() const override
  {
    return CHILD_PER_SLOT;
  }
  uint64_t _level_granularity() const override
  {
    return l2_granularity;
  }

  void _init(uint64_t capacity, uint64_t _alloc_unit)
  {
    l1._init(capacity, _alloc_unit);

    l2_granularity =
      l1._level_granularity() * l1._children_per_slot() * slotset_width;
    size_t elem_count = capacity / l2_granularity / CHILD_PER_SLOT;
    // we use set bit(s) as a marker for (partially) free entry
    l2.resize(elem_count, all_slot_set);
  }

  void _mark_l2_on_l1(int64_t l2_pos, int64_t l2_pos_end)
  {
    auto d = CHILD_PER_SLOT;
    assert(0 <= l2_pos_end);
    assert((int64_t)l2.size() >= (l2_pos_end / d));

    auto idx = l2_pos * slotset_width;
    auto idx_end = l2_pos_end * slotset_width;
    bool all_allocated = true;
    while (idx < idx_end) {
      if (!l1._is_slot_fully_allocated(idx)) {
        all_allocated = false;
        idx = p2roundup(int64_t(++idx), int64_t(slotset_width));
      }
      else {
        ++idx;
      }
      if ((idx % slotset_width) == 0) {
        if (all_allocated) {
          l2[l2_pos / d] &= ~(slot_t(1) << (l2_pos % d));
        }
        else {
          l2[l2_pos / d] |= (slot_t(1) << (l2_pos % d));
        }
        all_allocated = true;
        ++l2_pos;
      }
    }
  }

  void _allocate_l2(uint64_t length, uint64_t min_length,
    uint64_t pos_start, uint64_t pos_end,
    uint64_t* allocated,
    interval_list_t* res)
  {
    uint64_t d = CHILD_PER_SLOT;
    assert((pos_start % d) == 0);
    assert((pos_end % d) == 0);
    assert(min_length <= l2_granularity);

    uint64_t l1_w = slotset_width * l1._children_per_slot();
    auto l2_pos = pos_start;
    pos_start /= d;
    pos_end /= d;
    std::lock_guard<std::mutex> l(lock);
    for (auto pos = pos_start; pos < pos_end && length > *allocated;
      ++pos) {
      slot_t& slot_val = l2[pos];
      int free_pos = 0;
      bool all_set = false;
      if (slot_val == all_slot_clear) {
        l2_pos += d;
        continue;
      } else if (slot_val == all_slot_set) {
        free_pos = 0;
        all_set = true;
      } else {
        free_pos = find_next_set_bit(slot_val, free_pos);
        assert(free_pos >= 0 && free_pos < bits_per_slot);
      }
      do {
        assert(length > *allocated);
        bool empty = l1._allocate_l1(length - *allocated, min_length,
          (l2_pos + free_pos)* l1_w,
          (l2_pos + free_pos + 1) * l1_w,
          allocated,
          res);
        if (empty) {
          slot_val &= ~(slot_t(1) << free_pos);
        }
        if (length <= *allocated || slot_val == all_slot_clear) {
          break;
        }
        ++free_pos;
        if (!all_set) {
          free_pos = find_next_set_bit(slot_val, free_pos);
        }
      } while (free_pos < CHILD_PER_SLOT);
      l2_pos += d;
    }
  }

  void _free_l2(const interval_list_t& r)
  {
    auto l1_gran = l1._level_granularity();
    std::lock_guard<std::mutex> l(lock);
    for (auto i = 0; i < r.size(); ++i) {
      l1._free_l1(r[i]);
      uint64_t l2_pos = r[i].first / l2_granularity;
      uint64_t l2_pos_end = p2roundup(int64_t(r[i].first + r[i].second), int64_t(l2_granularity)) / l2_granularity;

      _mark_l2_on_l1(l2_pos, l2_pos_end);
    }
  }
};

#endif
