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

void alloc_l1_test()
{
  TestAllocatorLevel01 al1;
  al1.init(256 * 512 * 4096, 0x1000);
  
  auto i1 = al1.allocate_l1(0x1000, 0x1000, 0, 256); 
  assert(i1.first == 0);
  assert(i1.second == 0x1000);
  auto i2 = al1.allocate_l1(0x1000, 0x1000, 0, 256);
  assert(i2.first == 0x1000);
  assert(i2.second == 0x1000);
  al1.free_l1(i2);
  al1.free_l1(i1);
  i1 = al1.allocate_l1(0x1000, 0x1000, 0, 256);
  assert(i1.first == 0);
  assert(i1.second == 0x1000);
  i2 = al1.allocate_l1(0x1000, 0x1000, 0, 256);
  assert(i2.first == 0x1000);
  assert(i2.second == 0x1000);
  al1.free_l1(i1);
  al1.free_l1(i2);

  i1 = al1.allocate_l1(0x2000, 0x1000, 0, 256);
  assert(i1.first == 0);
  assert(i1.second == 0x2000);

  i2 = al1.allocate_l1(0x3000, 0x1000, 0, 256);
  assert(i2.first == 0x2000);
  assert(i2.second == 0x3000);

  al1.free_l1(i1);
  al1.free_l1(i2);

  i1 = al1.allocate_l1(0x2000, 0x1000, 0, 256);
  assert(i1.first == 0);
  assert(i1.second == 0x2000);

  i2 = al1.allocate_l1(2 * 1024 * 1024, 0x1000, 0, 256);
  assert(i2.first == 2 * 1024 * 1024);
  assert(i2.second == 2 * 1024 * 1024);

  al1.free_l1(i1);
  i1 = al1.allocate_l1(1024 * 1024, 0x1000, 0, 256);
  assert(i1.first == 0);
  assert(i1.second == 1024 * 1024);

  auto i3 = al1.allocate_l1(1024 * 1024 + 0x1000, 0x1000, 0, 256);
  assert(i3.first == 2 * 2 * 1024 * 1024);
  assert(i3.second == 1024 * 1024 + 0x1000);

  al1.free_l1(i1);
  al1.free_l1(i2);
  al1.free_l1(i3);
  printf("Done\n");
}