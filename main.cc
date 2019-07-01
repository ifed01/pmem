#include "persistent_objects.h"
#include "fastbmap_allocator_impl.h"

#include <iostream>
#include <stdio.h>
#include <string.h>

using namespace std;
using namespace PersistentObjects;

PERSISTENT_CLASS(A)
{
public:
  int n1 = 0, n2 = 0;
  char s[128] = "";
  A() {
  }
  A(int n) : n1(n), n2(n) {
  }
  PERSISTENT_DEAD
  {
  }
};

std::ostream& operator<<(std::ostream& out, const A& a)
{
  out << "A(" << a.n1 << ", " << a.n2 << ", '" << a.s;
  return out << "')";
}

PERSISTENT_CLASS(B)
{
public:
  int n1 = 0, n2 = 0;
  APtr a = nullptr;
  AUniquePtr aa = nullptr;
  B(APtr _a = nullptr) : a(_a)
  {
  }
  PERSISTENT_DEAD
  {
    PERSISTENT_DIE(a);
    PERSISTENT_DIE(aa);

  }
};

std::ostream& operator<<(std::ostream& out, const B& b)
{
  out << "B(" << b.n1 << ", " << b.n2 << ", ";
  const auto *ar = !b.a.is_null() ? b.a->inspect() : nullptr;
  if (ar) {
    out << *ar;
  } else {
    out << "nullptr";
  }
  out << ", ";
  const auto *aar = !b.aa.is_null() ? b.aa.inspect() : nullptr;
  if (aar) {
    out << *aar;
  }
  else {
    out << "nullptr";
  }
  return out << ")";
}

PERSISTENT_CLASS(C)
{
public:
  int a = 0, b = 0;
  persistent_vector2<APtr> av;
  persistent_vector2<uint64_t> iv;
  persistent_list2<APtr> al;
  persistent_list2<uint64_t> il;
  persistent_map2<uint64_t, uint64_t> im;
  persistent_map2<int, BPtr> bm;
  PERSISTENT_DEAD
  {
    for (auto aptr : av) {
      PERSISTENT_DIE(aptr);
    }
    for (auto aptr : al) {
      PERSISTENT_DIE(aptr);
    }
    for (auto bptr : bm) {
      PERSISTENT_DIE(bptr.second);
    }
  }
};

std::ostream& operator<<(std::ostream& out, const C& c)
{
  return out << "C(" << c.a << ", " << c.b
    << ", av(" << c.av.size() << " entries)"
    << ", iv(" << c.iv.size() << " entries)"
    << ", al(" << c.al.size() << " entries)"
    << ", il(" << c.il .size() << " entries)"
    << ", im(" << c.im.size() << " entries)"
    << ", bm(" << c.bm.size() << " entries)"
    << ")";
}

/*void alloc_l1_test();
void alloc_l2_test();
void alloc_l2_huge_test();
void alloc_l2_unaligned_test();
int main()
{
  alloc_l1_test();
  alloc_l2_unaligned_test();
  alloc_l2_test();
  alloc_l2_huge_test();
  std::cout << ">> Press 'Enter' to proceed..." << std::endl;
  getchar();
  return 0;
}*/
int main()
{
  root->init();

  const size_t obj_log_size = 64 * 1024;
  const size_t alloc_log_size = 64 * 1024;
  const size_t alloc_log_squeeze_threshold = 32;
  uint64_t capacity = 1024 * 1024 * 1024;
  uint32_t min_alloc_unit = MIN_OBJECT_SIZE;
  assert(min_alloc_unit == 16);

  TransactionRoot* tr_ptr = TransactionRoot::create(capacity);
  TransactionRoot& tr = *tr_ptr;

  tr.prepare(
    alloc_log_size,
    alloc_log_squeeze_threshold,
    obj_log_size,
    capacity,
    min_alloc_unit);
  std::cout << "alloc log size = " << tr.get_alog_size() << std::endl;
  std::cout << "available size = " << tr.get_available() << std::endl;
  assert(tr.get_object_count() == 0);

  {
    tr.start_read_access();
    APtr a;
    assert(a.is_null());
    tr.stop_read_access();
  }

  BPtr b;
  {
    assert(b.is_null());
    tr.start_transaction();

    b = BPtr::alloc_persistent_obj<B>(tr);
    assert(!b.is_null());
    const B* br0 = b->inspect();
    assert(br0->n1 == 0);
    assert(br0->n2 == 0);
    assert(br0->a.is_null());
    B* br = b->access(tr);
    assert(br0 == br); // as we just created b within this transaction
    br->n1++;
    br->n2 += 3;
    br->a = APtr::alloc_persistent_obj<A>(tr, 10);
    assert(br->n1 == 1);
    assert(br->n2 == 3);
    assert(!br->a.is_null());
    tr.rollback_transaction();
    assert(tr.get_object_count() == 0);
    // since b is not in persistent mem we need to rollback it manually
    b = nullptr;
  }
  {
    tr.start_transaction();
    b = BPtr::alloc_persistent_obj<B>(tr);
    assert(!b.is_null());
    const B* br0 = b->inspect();
    assert(br0->n1 == 0);
    assert(br0->n2 == 0);
    assert(br0->a.is_null());
    B* br = b->access(tr);
    assert(br0 == br); // as we just created b within this transaction
    auto* br2 = b->access(tr); // no op
    assert(br == br2);

    br->n1 += 5;
    br->n2++;
    br->a = APtr::alloc_persistent_obj<A>(tr, 20);
    assert(br->n1 == 5);
    assert(br->n2 == 1);
    assert(!br->a.is_null());
    tr.commit_transaction();
    assert(tr.get_object_count() != 0);
    {
      tr.start_read_access();
      const B* br0 = b->inspect();
      assert(br0->n1 == 5);
      assert(br0->n2 == 1);
      assert(!br0->a.is_null());
      const A* ar0 = br0->a->inspect();
      assert(ar0->n1 == 20);
      assert(ar0->n2 == 20);
      assert(strlen(ar0->s) == 0);

      std::cout << *b->inspect() << std::endl;
      tr.stop_read_access();
    }
  }

  {
    tr.start_transaction();
    const B* br0 = b->inspect();
    auto* br = b->access(tr);
    assert(br0 != br);
    br->n2 = 4321;
    if (br->a) {
      auto& ar0 = *br->a->inspect();
      auto& ar = *br->a->access(tr);
      assert(&ar0 != &ar);
      ar.n1 = 6;
      ar.n2 = 77;
      strncpy(ar.s, "test data", sizeof(ar.s));
    }
    tr.rollback_transaction();
    {
      tr.start_read_access();
      const B* br0 = b->inspect();
      assert(br0->n1 == 5);
      assert(br0->n2 == 1);
      assert(!br0->a.is_null());
      const A* ar0 = br0->a->inspect();
      assert(ar0->n1 == 20);
      assert(ar0->n2 == 20);
      assert(strlen(ar0->s) == 0);
      tr.stop_read_access();
    }
  }
  {
    tr.start_transaction();
    const B* br0 = b->inspect();
    auto* br = b->access(tr);
    assert(br0 != br);
    br->n1 *= 10;
    br->n2 = 2;
    if (br->a) {
      auto& ar0 = *br->a->inspect();
      auto& ar = *br->a->access(tr);
      assert(&ar0 != &ar);
      ar.n1 -= 3;
      ar.n2 = 0;
      strncpy(ar.s, "test data2", sizeof(ar.s));
    }
    tr.commit_transaction();
    {
      tr.start_read_access();
      const B* br0 = b->inspect();
      assert(br0->n1 == 50);
      assert(br0->n2 == 2);
      assert(!br0->a.is_null());
      const A* ar0 = br0->a->inspect();
      assert(ar0->n1 == 17);
      assert(ar0->n2 == 0);
      assert(strcmp(ar0->s, "test data2") == 0);
      std::cout << *b->inspect() << std::endl;
      tr.stop_read_access();
    }
  }

  {
    tr.start_transaction();
    auto* br = b->access(tr);
    if (br->a) {
      br->a->die(tr);
      br->a = nullptr;
    }
    br->n1 = 1234;
    tr.rollback_transaction();
    {
      tr.start_read_access();
      const B* br0 = b->inspect();
      assert(br0->n1 == 50);
      assert(br0->n2 == 2);
      assert(!br0->a.is_null());
      const A* ar0 = br0->a->inspect();
      assert(ar0->n1 == 17);
      assert(ar0->n2 == 0);
      assert(strcmp(ar0->s, "test data2") == 0);
      tr.stop_read_access();
    }
  }
  {
    tr.start_transaction();
    auto* br = b->access(tr);
    if (br->a) {
      br->a->die(tr);
      br->a = nullptr;
    }
    br->n1 *= 2;
    tr.commit_transaction();
    {
      tr.start_read_access();
      const B* br0 = b->inspect();
      assert(br0->n1 == 100);
      assert(br0->n2 == 2);
      assert(br0->a.is_null());
      std::cout << *b->inspect() << std::endl;
      tr.stop_read_access();
    }
  }

  {
    tr.start_transaction();
    auto* br = b->access(tr);
    assert(br->a.is_null());
    br->a = APtr::alloc_persistent_obj<A>(tr, 100);
    tr.commit_transaction();
  }
  {
    tr.start_transaction();
    b->die(tr);
    tr.rollback_transaction();
    {
      tr.start_read_access();
      const B* br0 = b->inspect();
      assert(br0->n1 == 100);
      assert(br0->n2 == 2);
      assert(!br0->a.is_null());
      const A* ar0 = br0->a->inspect();
      assert(ar0->n1 == 100);
      assert(ar0->n2 == 100);
      assert(strlen(ar0->s) == 0);
      std::cout << *b->inspect() << std::endl;
      tr.stop_read_access();
    }
  }
  {
    // trying access child without updating master obj
    tr.start_transaction();
    const B* br0 = b->inspect();
    auto ar = br0->a->access(tr);
    ar->n1 = 123;
    ar->n2 = 234;
    strncpy(ar->s, "test data123", sizeof(ar->s));
    tr.commit_transaction();
    tr.start_read_access();
    std::cout << *b->inspect() << std::endl;
    tr.stop_read_access();
  }
  {
    // allocate unique ptr
    // note "aa->allocate_obj" invocation which causes const_cast
    // via PUniquePtr::operator-> implementation
    tr.start_transaction();
    b->inspect()->aa->allocate_obj(tr, 200);
    tr.commit_transaction();

    //access unique ptr without updating master obj,
    // note "aa->access" invocation which causes const_cast
    // via PUniquePtr::operator-> implementation
    tr.start_transaction();
    auto aar = b->inspect()->aa->access(tr);
    aar->n1 *= 300;
    tr.commit_transaction();
    tr.start_read_access();
    std::cout << *b->inspect() << std::endl;
    tr.stop_read_access();
  }
  {
    tr.start_transaction();
    b->die(tr);
    tr.commit_transaction();
  }
  b = nullptr;
  std::cout << "alloc log size = " << tr.get_alog_size() << std::endl;
  std::cout << "available size = " << tr.get_available() << std::endl;
  std::cout << "obj count = " << tr.get_object_count() << std::endl;
  assert(tr.get_object_count() == 0);

  tr.restart();
  assert(tr.get_object_count() == 0);

  CPtr c = nullptr;
  {
    tr.start_transaction();
    c = CPtr::alloc_persistent_obj<C>(tr);
    C* cr = c->access(tr);
    cr->a++;
    cr->b++;
    cr->il.push_back(123);
    cr->iv.push_back(321);
    cr->im[1] = 333;
    cr->bm[1] = nullptr;
    tr.rollback_transaction();
    assert(tr.get_object_count() == 0);
    c = nullptr;
  }
  assert(tr.get_object_count() == 0);
  {
    tr.start_transaction();
    c = CPtr::alloc_persistent_obj<C>(tr);
    auto* cr = c->access(tr);
    cr->a++;
    cr->b++;
    cr->al.push_back(APtr());
    cr->il.push_back(9123);
    cr->av.push_back(nullptr);
    cr->iv.push_back(9321);

    (cr->im)[1] = 9333;
    (cr->bm)[1] = nullptr;
    tr.commit_transaction();
    assert(tr.get_object_count() != 0);
    {
      tr.start_read_access();
      const C* cr0 = c->inspect();
      assert(cr0->a == 1);
      assert(cr0->b == 1);
      assert(cr0->al.size() == 1);
      assert(cr0->al.front().is_null());
      assert(cr0->il.size() == 1);
      assert(cr0->il.front() == 9123);
      assert(cr0->av.size() == 1);
      assert(cr0->av[0].is_null());
      assert(cr0->iv.size() == 1);
      assert(cr0->iv[0] == 9321);
      assert(cr0->im.size() == 1);
      assert(cr0->im.at(1) == 9333);
      assert(cr0->bm.size() == 1);
      assert(cr0->bm.at(1).is_null());
      assert(cr0->im.find(0) == cr0->im.end());
      assert(cr0->im.find(1)->second == 9333);
      assert(cr0->bm.size() == 1);
      assert(cr0->bm.find(0) == cr0->bm.end());
      assert(cr0->bm.find(1)->second == BPtr(nullptr));
      std::cout << *cr << std::endl;
      tr.stop_read_access();
    }
  }

  {
    auto allocated = tr.get_object_count();
    tr.start_transaction();
    auto* cr = c->access(tr);
    cr->a++;
    cr->b++;

    cr->av.resize(2);
    cr->av[0] = APtr::alloc_persistent_obj<A>(tr, 55);
    cr->av[1] = APtr::alloc_persistent_obj<A>(tr, 56);
    cr->av.push_back(APtr::alloc_persistent_obj<A>(tr, 57));
    cr->iv.resize(11);
    for (size_t i = 0; i < 11; i++)
    {
      cr->iv[i] = 0;
    }
    cr->al.push_back(APtr::alloc_persistent_obj<A>(tr, 57));

    tr.rollback_transaction();
    assert(allocated == tr.get_object_count());
    {
      tr.start_read_access();
      const C* cr0 = c->inspect();
      assert(cr0->a == 1);
      assert(cr0->b == 1);
      assert(cr0->al.size() == 1);
      assert(cr0->al.front().is_null());
      assert(cr0->il.size() == 1);
      assert(cr0->il.front() == 9123);

      assert(cr0->av.size() == 1);
      assert(cr0->av[0].is_null());
      assert(cr0->iv.size() == 1);
      assert(cr0->iv[0] == 9321);
      assert(cr0->im.size() == 1);
      assert(cr0->im.at(1) == 9333);
      assert(cr0->bm.size() == 1);
      assert(cr0->bm.at(1).is_null());
      assert(cr0->im.find(0) == cr0->im.end());
      assert(cr0->im.find(1)->second == 9333);
      assert(cr0->bm.size() == 1);
      assert(cr0->bm.find(0) == cr0->bm.end());
      assert(cr0->bm.find(1)->second == BPtr(nullptr));
      std::cout << *cr0 << std::endl;
      tr.stop_read_access();
    }
  }
  {
    tr.start_transaction();
    auto* cr = c->access(tr);
    cr->a++;
    cr->b++;
    
    cr->av.resize(2);
    cr->av[0] = APtr::alloc_persistent_obj<A>(tr, 55);
    cr->av[1] = APtr::alloc_persistent_obj<A>(tr, 56);
    cr->av.push_back(APtr::alloc_persistent_obj<A>(tr, 57));
    cr->iv.resize(11);
    for (size_t i = 0; i < 11; i++)
    {
      cr->iv[i] = 0;
    }
    cr->al.push_back(APtr::alloc_persistent_obj<A>(tr, 58));
    cr->il.emplace_front(111);

    ++cr->im[1];
    ++cr->im[1];
    cr->im[100] = 100;

    cr->bm[2] = BPtr::alloc_persistent_obj<B>(tr,
      APtr::alloc_persistent_obj<A>(tr, 60));
    cr->bm[2]->access(tr)->n1 = 61;
    cr->bm[2]->access(tr)->a->access(tr)->n1 = 62;
    
    tr.commit_transaction();
    {
      tr.start_read_access();
      const C* cr0 = c->inspect();
      assert(cr0->a == 2);
      assert(cr0->b == 2);

      assert(cr0->al.size() == 2);
      assert(cr0->al.front().is_null());
      assert(cr0->al.back()->inspect()->n1 == 58);

      assert(cr0->il.size() == 2);
      assert(cr0->il.front() == 111);
      assert(*(++cr0->il.begin()) == 9123);

      assert(cr0->av.size() == 3);
      assert(cr0->av[0]->inspect()->n1 == 55);
      assert(cr0->av[1]->inspect()->n1 == 56);
      assert(cr0->av[2]->inspect()->n1 == 57);
      assert(cr0->iv.size() == 11);
      assert(cr0->iv[0] == 0);
      assert(cr0->iv[10] == 0);

      assert(cr0->im.size() == 2);
      assert(cr0->im.at(1) == 9335);
      assert(cr0->im.at(100) == 100);
      assert(cr0->im.find(0) == cr0->im.end());
      assert(cr0->im.find(1)->second == 9335);
      assert(cr0->im.find(100)->second == 100);

      assert(cr0->bm.size() == 2);
      assert(cr0->bm.at(1).is_null());
      assert(cr0->bm.at(2)->inspect()->n1 == 61);
      assert(cr0->bm.at(2)->inspect()->a->inspect()->n1 == 62);
      assert(cr0->bm.find(0) == cr0->bm.end());
      assert(cr0->bm.find(1)->second == BPtr(nullptr));
      assert(cr0->bm.find(2) != cr0->bm.end());
      std::cout << *cr0 << std::endl;
      tr.stop_read_access();
    }
  }
  {
    tr.start_transaction();
    auto* cr = c->inspect();
    auto* br = cr->bm.at(2)->access(tr);
    br->n1 += 10;
    br->n2 += 20;
    auto *ar = br->a->access(tr);
    ar->n1 += 30;
    //tr.rollback_transaction();
    std::cout << "alloc log size = " << tr.get_alog_size() << std::endl;
    std::cout << "available size = " << tr.get_available() << std::endl;
    tr.restart();

    std::cout << "alloc log size = " << tr.get_alog_size() << std::endl;
    std::cout << "available size = " << tr.get_available() << std::endl;
    {
      tr.start_read_access();
      const C* cr0 = c->inspect();

      assert(cr0->bm.size() == 2);
      assert(cr0->bm.at(1).is_null());
      assert(cr0->bm.at(2)->inspect()->n1 == 61);
      assert(cr0->bm.at(2)->inspect()->n2 == 0);
      assert(cr0->bm.at(2)->inspect()->a->inspect()->n1 == 62);
      std::cout << *cr0 << std::endl;
      tr.stop_read_access();
    }
  }
  {
    tr.start_transaction();
    auto* cr = c->inspect();
    auto* br = cr->bm.at(2)->access(tr);
    br->n1 += 10;
    br->n2 += 20;
    br->a->access(tr)->n1 += 30;

    tr.commit_transaction();
    {
      tr.start_read_access();
      const C* cr0 = c->inspect();

      assert(cr0->bm.size() == 2);
      assert(cr0->bm.at(1).is_null());
      assert(cr0->bm.at(2)->inspect()->n1 == 71);
      assert(cr0->bm.at(2)->inspect()->n2 == 20);
      assert(cr0->bm.at(2)->inspect()->a->inspect()->n1 == 92);
      std::cout << *cr0 << std::endl;
      tr.stop_read_access();
    }
  }
  {
    tr.start_transaction();
    auto* cr = c->inspect();
    std::cout << "C content prior to release:" << std::endl;
    std::cout << *cr << std::endl;

    c->die(tr);
    tr.commit_transaction();
  }
  std::cout << "alloc log size = " << tr.get_alog_size() << std::endl;
  std::cout << "available size = " << tr.get_available() << std::endl;
  std::cout << tr.get_object_count() << std::endl;
  assert(tr.get_object_count() == 0);                                                                                              

  // volatile pointers demo
  VPtr<int> v = new int(777);
  if (!v.is_null())
    std::cout << "*v = " << *v << std::endl;
  else
    std::cout << "v is null" << std::endl;

  // this simulates process restart hence invalidating volatile pointers
  tr.shutdown();
  root->restart();
  tr.restart();

  VPtr<int> v2(v);
  if (!v.is_null())
    std::cout << "*v = " << *v << std::endl;
  else
    std::cout << "v is null" << std::endl;
  if (!v2.is_null())
    std::cout << "*v2 = " << *v2 << std::endl;
  else
    std::cout << "v2 is null" << std::endl;

  v.reset(new int(779));
  if (!v.is_null())
    std::cout << "*v = " << *v << std::endl;
  else
    std::cout << "v is null" << std::endl;
  v2 = v;
  if (!v2.is_null())
    std::cout << "*v2 = " << *v2 << std::endl;
  else
    std::cout << "v2 is null" << std::endl;
  delete (int*)v;
//#endif
//  int aaa = 0;
//  offset_ptr<int> p(&aaa);
//  *p = 3;
//  std::cout << " aaa = " << *p << " " << aaa << std::endl;
  std::cout << "alloc log size = " << tr.get_alog_size() << std::endl;
  std::cout << "available size = " << tr.get_available() << std::endl;
  std::cout << "object count = " << tr.get_object_count() << std::endl;
  std::cout << ">> Press 'Enter' to proceed..." << std::endl;
  getchar();
  TransactionRoot::destroy(tr_ptr);
  return 0;
}
