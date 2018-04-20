#ifndef __ALLOCATOR_H
#define __ALLOCATOR_H
#include "intarith.h"

#include <assert.h>
#include <vector>
#include <algorithm>

typedef std::pair<uint64_t, size_t> interval_t;
typedef std::vector<interval_t> interval_list_t;
typedef uint64_t slot_t;

// fitting into cache line on x86_64
static const size_t slotset_width = 8; // 8 slots per set
static const size_t slotset_bytes = sizeof(slot_t) * slotset_width;
static const slot_t all_slot_set = 0xffffffffffffffff;
static const slot_t all_slot_clear = 0;


class AllocatorLevel
{
protected:

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
    size_t elem_count = capacity / _alloc_unit / sizeof(slot_t) / 8;
    l0.resize(elem_count, 0);
    l0_granularity = _alloc_unit;
    elem_count = elem_count / _children_per_slot();
    l1.resize(elem_count, 0);
    // 512 bits at L0 mapped to L1 entry
    l1_granularity = l0_granularity * slotset_bytes * 8;
  }
};

template <class T>
class AllocatorLevel02;

class AllocatorLevel01Loose : public AllocatorLevel01
{
  enum {
    L1_ENTRY_WIDTH = 2,
    L1_ENTRY_MASK = (1 << L1_ENTRY_WIDTH) - 1,
    L1_ENTRY_FREE = 0x00,
    L1_ENTRY_PARTIAL = 0x01,
    L1_ENTRY_FULL = 0x03,
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
          if (bits == 0) {
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
          if (bits == all_slot_set) {
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

      if ((bits & 1) == 0) {
        ++res_candidate;
        if (!was_free) {
          pos_free_start = pos;
          was_free = true;
        }
      } 
      end_loop = ++pos >= pos1;
      if (was_free && (end_loop || (bits & 1))) {
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

  bool _get_any_free_from_l0(uint64_t pos0, uint64_t pos1,
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
  }

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

  void _mark_l1_on_l0(int64_t l0_pos_start, int64_t l0_pos_end)
  {
    if (l0_pos_start == l0_pos_end) {
      return;
    }
    int64_t l0_w = slotset_width * CHILD_PER_SLOT_L0;
    int64_t l1_w = CHILD_PER_SLOT;

    int64_t pos = p2align(l0_pos_start, l0_w);
    int64_t pos_end = p2roundup(l0_pos_end, l0_w);

    int64_t l1_pos = pos / l0_w;
    int64_t prev_l1_pos = l1_pos;

    bool was_all_free = true;
    bool was_all_set = true;
    bool end_loop;
    do {
      auto& slot_val = l0[pos / CHILD_PER_SLOT_L0];
      pos += CHILD_PER_SLOT_L0;

      if (slot_val == all_slot_set) {
        was_all_free = false;
      } else if (slot_val == 0) {
        was_all_set = false;
      } else {
        was_all_free = false;
        was_all_set = false;
        // no need to check the current slot set, it's partial
        pos = p2roundup(pos, l0_w);
      }

      end_loop = pos >= pos_end;
      l1_pos = pos / l0_w;
      if (l1_pos != prev_l1_pos || end_loop) {
        uint64_t shift = (prev_l1_pos % l1_w) * L1_ENTRY_WIDTH;
        l1[prev_l1_pos / l1_w] &= ~(uint64_t(L1_ENTRY_MASK) << shift);
        if (was_all_set) {
          assert(!was_all_free);
          l1[prev_l1_pos / l1_w] |= uint64_t(L1_ENTRY_FULL) << shift;
        } else if (was_all_free) {
          assert(!was_all_set);
          l1[prev_l1_pos / l1_w] |= uint64_t(L1_ENTRY_FREE) << shift;
        } else {
          l1[prev_l1_pos / l1_w] |= uint64_t(L1_ENTRY_PARTIAL) << shift;
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
      l0[pos / d0] = all_slot_set;
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
      l0[pos / d0] = all_slot_clear;
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

public:
  uint64_t debug_get_free() const
  {
    uint64_t res = 0;
    for (uint64_t i = 0; i < l0.size(); ++i) {
      auto v = l0[i];
      if (v == all_slot_clear) {
        res += CHILD_PER_SLOT_L0;
      }
      else if (v != all_slot_set) {
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
        res += (sizeof(slot_t) * 8) - cnt;
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
protected:
  L1 l1;
  std::vector<slot_t> l2;
  uint64_t l2_granularity = 0; // space per entry

  enum {
    CHILD_PER_SLOT = sizeof(slot_t) * 8, // 64
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

    l2_granularity = l1._level_granularity() * slotset_bytes * 8;
    size_t elem_count = capacity / l2_granularity / sizeof(slot_t) / 8;
    // we use set bit(s) as a marker for (partially) free entry
    l2.resize(elem_count, all_slot_set);
  }

  void _allocate_l2(uint64_t length, uint64_t min_length,
    uint64_t pos_start, uint64_t pos_end,
    uint64_t* allocated,
    interval_list_t* res)
  {
    /*uint64_t d = CHILD_PER_SLOT;
    assert((pos_start % d) == 0);
    assert((pos_end % d) == 0);
    assert(min_length != _level_granularity());

    uint64_t l1_w = slotset_width * l1._children_per_slot();
    auto l2_pos = pos_start;
    pos_start /= d;
    pos_end /= d;
    for (auto pos = pos_start; pos < pos_end && length < *allocated;
      ++pos) {
      slot_t& slot_val = l2[pos];
      int free_pos = 0;
      if (slot_val == all_slot_clear) {
        l2_pos += d;
        continue;
      } else if (slot_val == all_slot_set) {
        free_pos = 0;
      } else {
#ifdef __GNUC__
        free_pos = __builtin_ffsll(slot_val);
        assert(free_pos);
        free_pos--;
#else
        slot_t mask = 1;
        while (free_pos < (sizeof(slot_t) * 8)) {
          if (slot_val & mask) {
            break;
          }
          ++free_pos;
          mask <<= 1;
        }
#endif
      }
      
      bool empty;
      do {
        empty = l1._allocate_l1(length - *allocated, min_length,
          l2_pos * l1_w,
          (l2_pos + 1) * l1_w,
          allocated,
          res);
      } while (length > *allocated && !empty);
      if (empty) {
        slot_val &= ~(slot_t(1) << free_pos);
      }
      l2_pos += free_pos;
    }*/
  }

  void _free_l2(const interval_list_t& r)
  {
  }
};

#endif
