#ifndef __ALLOCATOR_H
#define __ALLOCATOR_H
#include "intarith.h"

#include <vector>

class PersistentAllocator
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
    REGULAR_BITS = 2, // per entry
  };
  // entries per slot, all rows but 0th use 2(REGULAR) bits
  static const size_t slot_width_bottom = sizeof(slot_t) * 8 / BOTTOM_BITS;
  static const size_t slot_width = slot_width_bottom / REGULAR_BITS;
  static const slot_t all_slot_set = 0xffffffffffffffff;

  typedef std::vector<slot_t> row_t;
  typedef std::vector<row_t> bitmap_t;
  bitmap_t bm;
  std::vector<uint64_t> row_granularity;

  enum {
    DEFAULT,
  } cur_strategy = DEFAULT;
  enum {
    UNALLOCATED = 0,
    ALLOCATED_LOOSE = 1,
    ALLOCATED_NARROW = 2,
    ALLOCATED_BUSY = 3,
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

  void _get_side_intervals(size_t row, size_t pos, interval_t* left, interval_t* right);
  
  interval_t _choose_intervals(interval_t& i1, interval_t i2, size_t length, size_t min_alloc_size)
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
            res = std::make_pair(_mk_offs(row, pos0, o), length);
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
      return best_interval;
    }

    return res;
  }
};

#endif
