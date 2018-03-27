#include "persistent_objects.h"

#include <iostream>
#include <stdio.h>
#include <string.h>

using namespace PersistentObjects;

CLASS_PERSISTENT(A)
{
public:
  int n1 = 0, n2 = 0;
  char s[128] = "";
  A() {
  }
  A(int n) : n1(n), n2(n) {
  }
};

std::ostream& operator<<(std::ostream& out, const A& a)
{
  out << "A(" << a.n1 << ", " << a.n2 << ", '" << a.s;
  return out << "')";
}

CLASS_PERSISTENT(B)
{
public:
  int n1 = 0, n2 = 0;
  APtr a = nullptr;

};

std::ostream& operator<<(std::ostream& out, const B& b)
{
  out << "B(" << b.n1 << ", " << b.n2 << ", ";
  const auto *ar = b.a ? b.a->get() : nullptr;
  if (ar) {
    out << *ar;
  } else {
    out << "nullptr";
  }
  return out << ")";
}

CLASS_PERSISTENT(C)
{
public:
  int a, b;
  persistent_vector<APtr> av;
  persistent_vector<uint64_t> iv;
  persistent_list<APtr> al;
  persistent_list<uint64_t> il;
  C() : a(0), b(1)
  {
  }
};

std::ostream& operator<<(std::ostream& out, const C& c)
{
  return out << "C(" << c.a << ", " << c.b
    << ", av:" << c.av.size()
    << ", iv:" << c.iv.size()
    << ", al:" << c.al.size()
    << ", il:" << c.il .size()
    << ")";
}

int main()
{
  root->init();

  TransactionalRoot tr(1024);
  tr.restart();
  tr.start_transaction();
  BPtr b = alloc_persistent_obj<B>(tr);
  auto& br = *b->access(tr);
  br.n1++;
  br.n2++;
  br.a = alloc_persistent_obj<A>(tr, 50);
  
  //tr.rollback_transaction();
  tr.commit_transaction();
  std::cout << *b->get() << std::endl;

  tr.start_transaction();
  {
    auto* br = b->access(tr);
    br->n2 = 4321;
    if (br->a) {
      auto& ar = *br->a->access(tr);
      std::cout << (uint64_t)&ar << std::endl;
      ar.n1 = 6;
      ar.n2 = 77;
      strncpy(ar.s, "rtetery", sizeof(ar.s));
    }
  }
  tr.commit_transaction();
  //tr.rollback_transaction();
  std::cout << *b->get() << std::endl;

  tr.start_transaction();
  {
    auto* br = b->access(tr);
    if (br->a) {
      br->a->release(tr);
      br->a = nullptr;
    }
    br->n1 = 1234;
    auto* br2 = b->access(tr); // no op
    assert(br == br2);
  }
  tr.commit_transaction();
  //tr.rollback_transaction();
  std::cout << *b->get() << std::endl;

  tr.start_transaction();
  {
    auto* br0 = b->get();
    if ((*b)->a) {
      auto* br = b->access(tr);
      br->a->release(tr);
      br->a = nullptr;
    }
    b->release(tr);
  }
  tr.commit_transaction();

  CPtr c = nullptr;
  tr.start_transaction();

  {
    c = alloc_persistent_obj<C>(tr);
    auto* cr = c->access(tr);
    cr->a++;
    cr->b++;
  }
  //tr.rollback_transaction();
  tr.commit_transaction();
  std::cout << *c->get() << std::endl;

  tr.start_transaction();
  {
    auto* cr = c->access(tr);
    cr->a++;
    cr->b++;
    cr->av.resize(2);
    cr->av[0] = alloc_persistent_obj<A>(tr, 55);
    cr->av[1] = alloc_persistent_obj<A>(tr, 56);
    cr->av.push_back(alloc_persistent_obj<A>(tr, 57));
    cr->iv.resize(11);
    for (size_t i = 0; i < 11; i++)
    {
      cr->iv[i] = 0;
    }
    cr->al.push_back(APtr());
    cr->il.push_back(144);
    cr->il.push_back(145);
  }

  //tr.rollback_transaction();
  tr.commit_transaction();
  std::cout << *c->get() << std::endl;

  tr.start_transaction();
  {
    auto* cr = c->get();
    auto* av = cr->av[0]->access(tr);
    av->n1 = 123;
    assert(cr->al.front() == APtr());
  }

  //tr.rollback_transaction();
  tr.commit_transaction();
  std::cout << *c->get() << std::endl;

  tr.start_transaction();
  {
    auto* cr = c->get();
    std::cout << "C content prior to release: av: ";
    for (auto i = 0; i != cr->av.size(); ++i) {
      std::cout << (*cr->av[i])->n1 << ",";
      cr->av[i]->release(tr);
    }
    std::cout << " il:";
    for (auto e : cr->il) {
      std::cout << e << ",";
    }
    std::cout << std::endl;

    c->release(tr);
  }
  //tr.rollback_transaction();
  tr.commit_transaction();

  // volatile pointers demo
  VPtr<int> v = new int(777);
  if (v != nullptr)
    std::cout << "*v = " << *v << std::endl;
  else
    std::cout << "v is null" << std::endl;

  // this simulates process restart hence invalidating volatile pointers
  root->restart();
  VPtr<int> v2(v);
  if (v != nullptr)
    std::cout << "*v = " << *v << std::endl;
  else
    std::cout << "v is null" << std::endl;
  if (v2 != nullptr)
    std::cout << "*v2 = " << *v2 << std::endl;
  else
    std::cout << "v2 is null" << std::endl;

  v.reset(new int(779));
  if (v != nullptr)
    std::cout << "*v = " << *v << std::endl;
  else
    std::cout << "v is null" << std::endl;
  v2 = v;
  if (v2 != nullptr)
    std::cout << "*v2 = " << *v2 << std::endl;
  else
    std::cout << "v2 is null" << std::endl;
  delete (int*)v;

  getchar();
  return 0;
}
