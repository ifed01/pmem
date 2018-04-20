#include <iostream>
#include <stdio.h>

#include "allocator.h"

class TestAllocatorLevel01 : public AllocatorLevel01Loose
{
public:
  void init(uint64_t capacity, uint64_t alloc_unit)
  {
    _init(capacity, alloc_unit);
  }
  interval_t allocate_l1(uint64_t length, uint64_t min_length, uint64_t pos_start, uint64_t pos_end)
  {
    return _allocate_l1(length, min_length, pos_start, pos_end);
  }
  void free_l1(const interval_t& r)
  {
    _free_l1(r);
  }
};

class TestAllocatorLevel02 : public AllocatorLevel02<AllocatorLevel01Loose>
{
public:
  void init(uint64_t capacity, uint64_t alloc_unit)
  {
    _init(capacity, alloc_unit);
  }
  /*interval_t allocate_l2(uint64_t length, uint64_t min_length, uint64_t pos_start, uint64_t pos_end)
  {
    return _allocate_l2(length, min_length, pos_start, pos_end);
  }
  void free_l2(const interval_t& r)
  {
    _free_l2(r);
  }*/
};

void alloc_l1_test()
{
  TestAllocatorLevel01 al1;
  uint64_t num_l1_entries = 3 * 256;
  uint64_t capacity = num_l1_entries * 512 * 4096;
  al1.init(capacity, 0x1000);
  assert(capacity == al1.debug_get_free());

  auto i1 = al1.allocate_l1(0x1000, 0x1000, 0, num_l1_entries); 
  assert(i1.first == 0);
  assert(i1.second == 0x1000);
  assert(capacity - 0x1000 == al1.debug_get_free());

  auto i2 = al1.allocate_l1(0x1000, 0x1000, 0, num_l1_entries);
  assert(i2.first == 0x1000);
  assert(i2.second == 0x1000);
  al1.free_l1(i2);
  al1.free_l1(i1);
  i1 = al1.allocate_l1(0x1000, 0x1000, 0, num_l1_entries);
  assert(i1.first == 0);
  assert(i1.second == 0x1000);
  i2 = al1.allocate_l1(0x1000, 0x1000, 0, num_l1_entries);
  assert(i2.first == 0x1000);
  assert(i2.second == 0x1000);
  al1.free_l1(i1);
  al1.free_l1(i2);

  i1 = al1.allocate_l1(0x2000, 0x1000, 0, num_l1_entries);
  assert(i1.first == 0);
  assert(i1.second == 0x2000);

  i2 = al1.allocate_l1(0x3000, 0x1000, 0, num_l1_entries);
  assert(i2.first == 0x2000);
  assert(i2.second == 0x3000);

  al1.free_l1(i1);
  al1.free_l1(i2);

  i1 = al1.allocate_l1(0x2000, 0x1000, 0, num_l1_entries);
  assert(i1.first == 0);
  assert(i1.second == 0x2000);

  i2 = al1.allocate_l1(2 * 1024 * 1024, 0x1000, 0, num_l1_entries);
  assert(i2.first == 2 * 1024 * 1024);
  assert(i2.second == 2 * 1024 * 1024);

  al1.free_l1(i1);
  i1 = al1.allocate_l1(1024 * 1024, 0x1000, 0, num_l1_entries);
  assert(i1.first == 0);
  assert(i1.second == 1024 * 1024);

  auto i3 = al1.allocate_l1(1024 * 1024 + 0x1000, 0x1000, 0, num_l1_entries);
  assert(i3.first == 2 * 2 * 1024 * 1024);
  assert(i3.second == 1024 * 1024 + 0x1000);

  // here we have the following layout:
  // Alloc: 0~1M, 2M~2M, 4M~1M+4K 
  // Free: 1M~1M, 4M+4K ~ 2M-4K, 6M ~...
  //
  auto i4 = al1.allocate_l1(1024 * 1024, 0x1000, 0, num_l1_entries);
  assert(i4.first == 1 * 1024 * 1024);
  assert(i4.second == 1024 * 1024);
  al1.free_l1(i4);

  i4 = al1.allocate_l1(1024 * 1024 - 0x1000, 0x1000, 0, num_l1_entries);
  assert(i4.first == 5 * 1024 * 1024 + 0x1000);
  assert(i4.second == 1024 * 1024 - 0x1000);
  al1.free_l1(i4);

  i4 = al1.allocate_l1(1024 * 1024 + 0x1000, 0x1000, 0, num_l1_entries);
  assert(i4.first == 6 * 1024 * 1024);
  //assert(i4.first == 5 * 1024 * 1024 + 0x1000);
  assert(i4.second == 1024 * 1024 + 0x1000);

  al1.free_l1(i1);
  al1.free_l1(i2);
  al1.free_l1(i3);
  al1.free_l1(i4);

  i1 = al1.allocate_l1(1024 * 1024, 0x1000, 0, num_l1_entries);
  assert(i1.first == 0);
  assert(i1.second == 1024 * 1024);

  i2 = al1.allocate_l1(1024 * 1024, 0x1000, 0, num_l1_entries);
  assert(i2.first == 1 * 1024 * 1024);
  assert(i2.second == 1024 * 1024 );

  i3 = al1.allocate_l1(512 * 1024, 0x1000, 0, num_l1_entries);
  assert(i3.first == 2 * 1024 * 1024);
  assert(i3.second == 512 * 1024);

  i4 = al1.allocate_l1(1536 * 1024, 0x1000, 0, num_l1_entries);
  assert(i4.first == (2 * 1024 + 512) * 1024 );
  assert(i4.second == 1536 * 1024);
  // making a hole 1.5 Mb length
  al1.free_l1(i2);
  al1.free_l1(i3);
  // and trying to fill it
  i2 = al1.allocate_l1(1536 * 1024, 0x1000, 0, num_l1_entries);
  assert(i2.first == 1024 * 1024);
  assert(i2.second == 1536 * 1024);

  al1.free_l1(i2);
  // and trying to fill it partially
  i2 = al1.allocate_l1(1528 * 1024, 0x1000, 0, num_l1_entries);
  assert(i2.first == 1024 * 1024);
  assert(i2.second == 1528 * 1024);

  i3 = al1.allocate_l1(8 * 1024, 0x1000, 0, num_l1_entries);
  assert(i3.first == 2552 * 1024);
  assert(i3.second == 8 * 1024);

  al1.free_l1(i2);
  // here we have the following layout:
  // Alloc: 0~1M, 2552K~8K, num_l1_entries0K~1.5M 
  // Free: 1M~1528K, 4M ~...
  //
  i2 = al1.allocate_l1(1536 * 1024, 0x1000, 0, num_l1_entries);
  assert(i2.first == 4 * 1024 * 1024);
  assert(i2.second == 1536 * 1024);

  al1.free_l1(i1);
  al1.free_l1(i2);
  al1.free_l1(i3);
  al1.free_l1(i4);
  assert(capacity == al1.debug_get_free());

  uint64_t _1m = 1024 * 1024;
  uint64_t _2m = 2 * 1024 * 1024;
  for (uint64_t i = 0; i < capacity; i += _2m) {
    i1 = al1.allocate_l1(_2m, _2m, 0, num_l1_entries);
    assert(i1.first == i);
    assert(i1.second == _2m);
  }
  assert(0 == al1.debug_get_free());
  i2 = al1.allocate_l1(_2m, _2m, 0, num_l1_entries);
  assert(i2.second == 0);
  assert(0 == al1.debug_get_free());

  al1.free_l1(i1);
  i2 = al1.allocate_l1(_2m, _2m, 0, num_l1_entries);
  assert(i2 == i1);
  al1.free_l1(i2);
  i2 = al1.allocate_l1(_1m, _1m, 0, num_l1_entries);
  assert(i2.first == i1.first);
  assert(i2.second == _1m);

  i3 = al1.allocate_l1(_2m, _2m, 0, num_l1_entries);
  assert(i3.second == 0);

  i3 = al1.allocate_l1(_2m, _1m, 0, num_l1_entries);
  assert(i3.second == _1m);

  i4 = al1.allocate_l1(_2m, _1m, 0, num_l1_entries);
  assert(i4.second == 0);

  al1.free_l1(i2);
  i2 = al1.allocate_l1(_2m, _2m, 0, num_l1_entries);
  assert(i2.second == 0);

  i2 = al1.allocate_l1(_2m, 0x1000, 0, num_l1_entries);
  assert(i2.second == _1m);

  al1.free_l1(i2);
  al1.free_l1(i3);
  assert(_2m == al1.debug_get_free());

  i1 = al1.allocate_l1(_2m - 3 * 0x1000, 0x1000, 0, num_l1_entries);
  i2 = al1.allocate_l1(0x1000, 0x1000, 0, num_l1_entries);
  i3 = al1.allocate_l1(0x1000, 0x1000, 0, num_l1_entries);
  i4 = al1.allocate_l1(0x1000, 0x1000, 0, num_l1_entries);
  assert(0 == al1.debug_get_free());

  al1.free_l1(i2);
  al1.free_l1(i4);

  i2 = al1.allocate_l1(0x4000, 0x2000, 0, num_l1_entries);
  assert(i2.second == 0);
  i2 = al1.allocate_l1(0x4000, 0x1000, 0, num_l1_entries);
  assert(i2.second == 0x1000);
  
  al1.free_l1(i3);
  i3 = al1.allocate_l1(0x6000, 0x3000, 0, num_l1_entries);
  assert(i3.second == 0);
  i3 = al1.allocate_l1(0x6000, 0x1000, 0, num_l1_entries);
  assert(i3.second == 0x2000);
  assert(0 == al1.debug_get_free());

  printf("Done\n");

}

void alloc_l2_test()
{
  TestAllocatorLevel02 al2;
  uint64_t capacity = (uint64_t)512 * 256 * 512 * 4096;
  al2.init(capacity, 0x1000);

}