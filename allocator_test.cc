#undef NDEBUG
#include <assert.h>
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
  void allocate_l2(uint64_t length, uint64_t min_length,
    uint64_t pos_start, uint64_t pos_end,
    uint64_t* allocated,                                                           
    interval_list_t* res)
  {
    _allocate_l2(length, min_length, pos_start, pos_end, allocated, res);
  }
  void free_l2(const interval_list_t& r)
  {
    _free_l2(r);
  }
};

const uint64_t _1m = 1024 * 1024;
const uint64_t _2m = 2 * 1024 * 1024;

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

/*  disabled to speed up test case execution
  for (uint64_t i = 0; i < capacity; i += 0x1000) {
    i1 = al1.allocate_l1(0x1000, 0x1000, 0, num_l1_entries);
    assert(i1.first == i);
    assert(i1.second == 0x1000);
  }
  assert(0 == al1.debug_get_free());
  for (uint64_t i = 0; i < capacity; i += _2m) {
    al1.free_l1(interval_t(i, _2m));
  }
  assert(capacity == al1.debug_get_free());
*/ 

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

  printf("Done L1\n");
}

void alloc_l2_test()
{
  TestAllocatorLevel02 al2;
  uint64_t num_l2_entries = 64;// *512;
  uint64_t capacity = num_l2_entries * 256 * 512 * 4096;
  al2.init(capacity, 0x1000);
  printf("Init L2\n");

  uint64_t allocated1 = 0;
  interval_list_t a1;
  al2.allocate_l2(0x2000, 0x2000, 0, num_l2_entries, &allocated1, &a1);
  assert(allocated1 == 0x2000);
  assert(a1[0].first == 0);
  assert(a1[0].second == 0x2000);
  
  // limit query range in debug_get_free for the sake of performance
  assert(0x2000 == al2.debug_get_allocated(0, 1));  
  assert(0 == al2.debug_get_allocated(1, 2));

  uint64_t allocated2 = 0;
  interval_list_t a2;
  al2.allocate_l2(0x2000, 0x2000, 0, num_l2_entries, &allocated2, &a2);
  assert(allocated2 == 0x2000);
  assert(a2[0].first == 0x2000);
  assert(a2[0].second == 0x2000);
  // limit query range in debug_get_free for the sake of performance
  assert(0x4000 == al2.debug_get_allocated(0, 1));
  assert(0 == al2.debug_get_allocated(1, 2));

  al2.free_l2(a1);

  allocated2 = 0;
  a2.clear();
  al2.allocate_l2(0x1000, 0x1000, 0, num_l2_entries, &allocated2, &a2);
  assert(allocated2 == 0x1000);
  assert(a2[0].first == 0x0000);
  assert(a2[0].second == 0x1000);
  // limit query range in debug_get_free for the sake of performance
  assert(0x3000 == al2.debug_get_allocated(0, 1));
  assert(0 == al2.debug_get_allocated(1, 2));

  uint64_t allocated3 = 0;
  interval_list_t a3;
  al2.allocate_l2(0x2000, 0x1000, 0, num_l2_entries, &allocated3, &a3);
  assert(allocated3 == 0x2000);
  assert(a3.size() == 2);
  assert(a3[0].first == 0x1000);
  assert(a3[0].second == 0x1000);
  assert(a3[1].first == 0x4000);
  assert(a3[1].second == 0x1000);
  // limit query range in debug_get_free for the sake of performance
  assert(0x5000 == al2.debug_get_allocated(0, 1));
  assert(0 == al2.debug_get_allocated(1, 2));
  {
    interval_list_t r;
    r.emplace_back(0x0, 0x5000);
    al2.free_l2(r);
  }

#ifndef _DEBUG
  for (uint64_t i = 0; i < capacity; i += 0x1000) {
    uint64_t allocated4 = 0;
    interval_list_t a4;
    al2.allocate_l2(0x1000, 0x1000, 0, num_l2_entries, &allocated4, &a4);
    assert(a4.size() == 1);
    assert(a4[0].first == i);
    assert(a4[0].second == 0x1000);
    if (0 == (i % (1 * 1024 * _1m))) {
      std::cout << "alloc1 " << i / 1024 / 1024 << " mb of "
        << capacity / 1024 / 1024 << std::endl;
    }
  }
#else
  for (uint64_t i = 0; i < capacity; i += _2m) {
    uint64_t allocated4 = 0;
    interval_list_t a4;
    al2.allocate_l2(_2m, _2m, 0, num_l2_entries, &allocated4, &a4);
    assert(a4.size() == 1);
    assert(a4[0].first == i);
    assert(a4[0].second == _2m);
    if (0 == (i % (1 * 1024 * _1m))) {
      std::cout << "alloc1 " << i / 1024 / 1024 << " mb of " 
                << capacity / 1024 / 1024 << std::endl;
    }
  }
#endif

  assert(0 == al2.debug_get_free());
  for (uint64_t i = 0; i < capacity; i += _1m) {
    interval_list_t r;
    r.emplace_back(interval_t(i, _1m));
    al2.free_l2(r);
    if (0 == (i % (1 * 1024 * _1m))) {
      std::cout << "free1 " << i / 1024 / 1024 << " mb of "
        << capacity / 1024 / 1024 << std::endl;
    }
  }
  assert(capacity == al2.debug_get_free());
  
  for (uint64_t i = 0; i < capacity; i += _1m) {
    uint64_t allocated4 = 0;
    interval_list_t a4;
    al2.allocate_l2(_1m, _1m, 0, num_l2_entries, &allocated4, &a4);
    assert(a4.size() == 1);
    assert(allocated4 == _1m);
    assert(a4[0].first == i);
    assert(a4[0].second == _1m);
    if (0 == (i % (1 * 1024 * _1m))) {
      std::cout << "alloc2 " << i / 1024 / 1024 << " mb of "
        << capacity / 1024 / 1024 << std::endl;
    }
  }
  assert(0 == al2.debug_get_free());
  uint64_t allocated4 = 0;
  interval_list_t a4;
  al2.allocate_l2(_1m, _1m, 0, num_l2_entries, &allocated4, &a4);
  assert(a4.size() == 0);
  al2.allocate_l2(0x1000, 0x1000, 0, num_l2_entries, &allocated4, &a4);
  assert(a4.size() == 0);

  for (uint64_t i = 0; i < capacity; i += 0x2000) {
    interval_list_t r;
    r.emplace_back(interval_t(i, 0x1000));
    al2.free_l2(r);
    if (0 == (i % (1 * 1024 * _1m))) {
      std::cout << "free2 " << i / 1024 / 1024 << " mb of "
        << capacity / 1024 / 1024 << std::endl;
    }
  }
  assert(capacity / 2 == al2.debug_get_free());

  // unable to allocate due to fragmentation
  al2.allocate_l2(_1m, _1m, 0, num_l2_entries, &allocated4, &a4);
  assert(a4.size() == 0);

  for (uint64_t i = 0; i < capacity; i += 2 * _1m) {
    a4.clear();
    allocated4 = 0;
    al2.allocate_l2(_1m, 0x1000, 0, num_l2_entries, &allocated4, &a4);
    assert(a4.size() == _1m / 0x1000);
    assert(allocated4 == _1m);
    assert(a4[0].first == i);
    assert(a4[0].second == 0x1000);
    if (0 == (i % (1 * 1024 * _1m))) {
      std::cout << "alloc3 " << i / 1024 / 1024 << " mb of "
        << capacity / 1024 / 1024 << std::endl;
    }
  }
  assert(0 == al2.debug_get_free());

  printf("Done L2\n");
}
