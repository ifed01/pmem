#include "persistent_objects.h"

#include <assert.h>
#include <iostream>

using namespace PersistentObjects;

thread_local
TransactionalRoot* PersistentObjects::working_transactional_root = nullptr;

void PersistentObjects::set_transactional_root(TransactionalRoot* tr)
{
  working_transactional_root = tr;
}

  
PersistencyRoot rootInstance;
// FIXME: in fact we need to obtain that root from persistent store(pool)
PersistencyRoot* PersistentObjects::root = &rootInstance;


AllocEntry TransactionalAllocator::alloc(size_t uint8_ts)
{
  void* ptr = malloc(uint8_ts); // FIXME
  AllocEntry e;
  e.offset = (uint64_t)ptr;
  e.length = (uint32_t)uint8_ts; // FIXME: adjust length to take service and alignment uint8_ts into account?

  alloc_cnt++;
//  std::cout << "new(" << alloc_cnt << ") " << e.offset << "~" << e.length << std::endl;

  return e;
}

void TransactionalAllocator::free(const AllocEntry& e)
{
  alloc_cnt--;
  assert(e.length != 0);
//  std::cout << "del(" << alloc_cnt << ") " << e.offset << "~" << e.length << std::endl;
  return ::free((void*)e.offset); // FIXME
}

void TransactionalRoot::replay()
{
  // FIXME: check for consistency when failing here!!

  set_transactional_root(this);
  in_transaction = true;
  if (idPrev < idNext) {

    // throw away uncommitted part
    alloc_log_next = alloc_log_cur;

    while (obj_log_start < obj_log_end) {
      ObjLogEntry& o = obj_log[obj_log_start];
      o.obj->recover(o.tid, o.ptr);
      ++obj_log_start;
    }
    obj_log_end = 0;
    obj_log_start = 0;
    idPrev.store(idPrev);
  } else {
    assert(idPrev == idNext);

    // for sure - they can mismatch but that's safe
    // (see comments in commit_transaction)
    //
    alloc_log_cur = alloc_log_next;
    obj_log_end = 0;
    obj_log_start = 0;
  }
  auto i = alloc_log_start;
  while (i < alloc_log_next) {
    if (alloc_log[i].is_release()) {
      allocator.apply_release(alloc_log[i]);
    }
    else {
      allocator.note_alloc(alloc_log[i]);
    }
    ++i;
  }
  in_transaction = false;
  set_transactional_root(nullptr);
}

int TransactionalRoot::start_read_access()
{
  LOCK_READ;
  readers_count++;
  return 0;
}

int TransactionalRoot::stop_read_access()
{
  --readers_count;
  UNLOCK_READ;
  return 0;
}

int TransactionalRoot::start_transaction()
{
  LOCK;
  assert(idPrev <= idNext);
  assert(alloc_log_cur == alloc_log_next);
  assert(obj_log_start == obj_log_end);
  set_transactional_root(this);
  in_transaction = true;
  ++idNext;

  return 0;
}

int TransactionalRoot::commit_transaction()
{
  assert(idPrev < idNext);

  // NB: objects2release might grow during the enumeration
  for (size_t pos = 0; pos < objects2release->size(); pos++) {

    auto& d = objects2release->at(pos);
    if (d.destroy_fn) {
      reinterpret_cast<PObjBase*>(d.p)->destroy(*this, d.len, d.destroy_fn);
    } else {
      free_persistent_raw(d.p, d.len);
    }
  }
  objects2release->clear();
  in_transaction = false;
  set_transactional_root(nullptr);

  idPrev.store(idNext);

  // Need to handle in alloc_log reply code the case when we fail exatly at
  // this point. In fact we can just ignore the difference if transaction
  // is not in progress (idPrev == idNext)
  alloc_log_cur = alloc_log_next;

  // the same handling as above here - ignore the diff if no transaction
  // is in progress
  obj_log_end = 0;
  obj_log_start = 0;

  UNLOCK;
  return 0;
}

int TransactionalRoot::rollback_transaction()
{
  assert(idPrev < idNext);

  objects2release->clear();

  // revert allocations
  auto i = alloc_log_cur;
  while (i < alloc_log_next) {
    if (!alloc_log[i].is_release()) {
      allocator.free(alloc_log[i]);
    }
    ++i;
  }
  alloc_log_next = alloc_log_cur;

  while (obj_log_start < obj_log_end) {
    ObjLogEntry& o = obj_log[obj_log_start];
    o.obj->recover(o.tid, o.ptr);
    ++obj_log_start;
  }
  obj_log_end = 0;
  obj_log_start = 0;
  set_transactional_root(nullptr);

  idNext.store(idPrev);

  UNLOCK;
  return 0;
}

void TransactionalRoot::queue_in_progress(PObjRecoverable* obj, TransactionId tid, void* ptr)
{
  assert(obj_log_end < obj_log_size);
  ObjLogEntry& e = obj_log[obj_log_end];
  e = ObjLogEntry(obj, tid, ptr);
  obj_log_end++;
}

void* PObjBase::operator new(size_t sz, TransactionalRoot& tr, size_t dummy)
{
  return tr.alloc_persistent_raw(sz);
}

void PObjBase::operator delete(void* ptr, TransactionalRoot& tr, size_t len)
{
  tr.free_persistent_raw(ptr, len);
}
void PObjBase::destroy(TransactionalRoot& tr, size_t len, dtor destroy_fn)
{
  destroy_fn(this); // virtual dtor replacement as we can't use virtuals
  operator delete(this, tr, len);
}
