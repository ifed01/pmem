#include "persistent_objects.h"

#include <assert.h>
#include <iostream>

using namespace PersistentObjects;

thread_local
TransactionRoot* PersistentObjects::working_Transaction_root = nullptr;

void PersistentObjects::set_Transaction_root(TransactionRoot* tr)
{
  working_Transaction_root = tr;
}

  
PersistencyRoot rootInstance;
// FIXME: in fact we need to obtain that root from persistent store(pool)
PersistencyRoot* PersistentObjects::root = &rootInstance;

AllocEntry TransactionAllocator::alloc(size_t uint8_ts)
{
  const auto min_alloc = get_min_alloc_size();
  interval_vector_t v; // FIXME minor: introduce single interval alloc request to allocator and get rid off vector here
  uint64_t allocated = 0;
  auto l = p2roundup<uint64_t>(uint8_ts, min_alloc); // FIXME we might waste some space by doing this but bmap allocator requires min_alloc_size to be power of 2
  _allocate_l2(l, l, l, 0, &allocated, &v);
  assert(v.size() == 1);
  assert(allocated >= uint8_ts);
  AllocEntry e;
  e.offset = (uint64_t)v[0].offset;
  e.length = (uint32_t)l; 

  alloc_cnt++;
  return e;
}

uint64_t TransactionAllocator::alloc(
  size_t uint8_ts,
  size_t min_size,
  bufferlist& res) {
  uint64_t ret = 0;

  interval_vector_t intervals;
  uint64_t allocated = 0;
  _allocate_l2(uint8_ts, min_size, uint8_ts, 0, &allocated, &intervals);

  assert(allocated >= uint8_ts);
  res.resize(intervals.size());
  size_t i = 0;
  for (auto iv : intervals) {
    res[i].first = poffs2ptr<uint8_t>(iv.offset);
    res[i++].second = iv.length;
  }

  alloc_cnt += intervals.size();
  return allocated;
}

void TransactionAllocator::free(const AllocEntry& e)
{
  const auto min_alloc = get_min_alloc_size();
  interval_vector_t v(1); // FIXME minor: introduce single interval release request to allocator and get rid off vector here
  v[0].offset = e.offset;
  v[0].length = p2roundup<uint64_t>(e.length, min_alloc);
  _free_l2(v);

  alloc_cnt--;
}

void TransactionAllocator::free(const bufferlist& to_rel)
{
  interval_vector_t intervals(to_rel.size());

  size_t i = 0;
  for (auto iv : intervals) {

    iv.offset = ptr2poffs(to_rel[i].first);
    iv.length = to_rel[i].second;
  }
  _free_l2(intervals);
  alloc_cnt += intervals.size();
}

void TransactionAllocator::note_alloc(const AllocEntry& e)
{
  assert(initialized());
  const auto min_alloc = get_min_alloc_size();
  _mark_allocated(e.offset, p2roundup<uint64_t>(e.length, min_alloc));
  alloc_cnt++;
}

void TransactionAllocator::apply_release(const AllocEntry& e)
{
  assert(initialized());
  const auto min_alloc = get_min_alloc_size();
  _mark_free(e.offset, p2roundup<uint64_t>(e.length, min_alloc));
  alloc_cnt--;
}

void PBuffer::setup_new(TransactionRoot& t, uint64_t _offs, size_t new_size) {
  assert(tid != 0);

  auto _tid = t.get_effective_id();
  if (_tid != tid) {
    t.queue_in_progress(this, tid, offs);
    if (offs) {
      t.queue_for_release(offs, len);
    }
  }
  else {
    t.free_persistent_raw(offs, len);
  }

  tid = _tid;
  offs = _offs;
}

void PBuffer::die(TransactionRoot& t) {
  if (offs) {
    t.queue_in_progress(this, tid, offs);
    t.queue_for_release(offs, len);

    tid = 0;
    offs = 0;
  }
}

void TransactionRoot::AllocationLog::die(TransactionRoot& t)
{
  if (snapshot_bufferlist.is_null())
    return;

  const AllocEntry* buf_entry =
    reinterpret_cast<AllocEntry*>(snapshot_bufferlist.get());
  for (auto i = 0; i < snapshot_blist_size; i++) {
    t.queue_for_release(buf_entry->offset, buf_entry->length);
    ++buf_entry;
  }

  snapshot_bufferlist.die(t);
}

void TransactionRoot::AllocationLog::apply_allocator_snapshot(
  TransactionAllocator& alloc) {

  if (snapshot_bufferlist.is_null())
    return;

  bufferlist snapshot_buffers(snapshot_blist_size);
  const AllocEntry* buf_entry =
    reinterpret_cast<AllocEntry*>(snapshot_bufferlist.get());
  for (auto i = 0; i < snapshot_blist_size; i++) {
    snapshot_buffers[i].first = poffs2ptr<uint8_t>(buf_entry->offset);
    snapshot_buffers[i].second = buf_entry->length;
    ++buf_entry;
  }
  alloc.apply_snapshot(snapshot_buffers, snapshot_alloc_cnt);
}

AllocEntry TransactionRoot::AllocationLog::squeeze(TransactionRoot& t, TransactionAllocator& alloc) {
  AllocLogEntry first = at(0);
  assert(first.is_init());
  
  auto alloc_cnt0 = alloc.get_alloc_count();

  auto need_size = sizeof(AllocationLog);
  // NB: adjust by - 1 as sizeof(AllocationLog) takes one into account
  need_size += sizeof(AllocLogEntry) * (alloc_log_size - 1);

  AllocEntry self = alloc.alloc(need_size);
  assert(self.length >= need_size);

  AllocationLog* alog = poffs2ptr<AllocationLog>(self.offset);
  alog->alloc_log_size = alloc_log_size;
  alog->alloc_log_start = alog->alloc_log_cur = alog->alloc_log_next = 0;

  need_size = alloc.get_snapshot_size();
  bufferlist new_buffers;
  auto allocated = alloc.alloc(need_size, ALLOC_SNAPSHOT_PAGE, new_buffers);
  assert(allocated >= need_size);

  need_size = new_buffers.size() * sizeof(AllocEntry);
  AllocEntry snapshot_bufferlist = alloc.alloc(need_size);
  alog->snapshot_bufferlist.setup_initial(
    t.get_effective_id(),
    snapshot_bufferlist.offset,
    snapshot_bufferlist.length);

  size_t j = 0;
  AllocEntry* entries = reinterpret_cast<AllocEntry*>(alog->snapshot_bufferlist.get());
  for (auto i : new_buffers) {
    entries[j].offset = ptr2poffs(i.first);
    entries[j].length = (uint32_t)i.second;
    ++j;
  }
  alog->snapshot_blist_size = j;
  alog->snapshot_alloc_cnt = alloc.get_alloc_count();
  alog->next() = first;

  auto captured_capacity = alloc.take_snapshot(new_buffers);
  assert(captured_capacity >= alloc.get_capacity());
  alog->alloc_log_base_cnt = alloc.get_alloc_count() - alloc_cnt0;
  return self;
}

void TransactionRoot::replay()
{
  // FIXME: check for consistency when failing here!!

  set_Transaction_root(this);
  in_transaction = true;
  if (idPrev < idNext) {

    // throw away uncommitted part
    ((AllocationLog&)alloc_log).rollback();

    auto i = obj_log.start();
    while (i != obj_log.end()) {
      ObjLogEntry& o = *i;
      poffs2ptr<PObjRecoverable>(o.obj_offs)->recover(o.tid, o.offs);
      ++i;
    }
    obj_log.reset();
    idPrev.store(idPrev);
  } else {
    assert(idPrev == idNext);

    // for sure - they can mismatch but that's safe
    // (see comments in commit_transaction)
    //
    ((AllocationLog&)alloc_log).commit();
    obj_log.reset();
  }
  AllocationLog& alog = alloc_log;
  auto i = alog.start();
  while (i != alog.cur()) {
    if (i->is_init()) {
      // [ab]use alloc log entry members as capacity/min_alloc_unit
      allocator->init(i->offset, i->length, TR_ROOT_PREALLOC_SIZE);
      //FIXME: we'll need to init root base here once real PM is used 
      //assert(root->base == 0);
      //root->base = allocator.get_capacity(); // FIXME: access and allocate PMem. Do mmap?
      alog.apply_allocator_snapshot(*allocator);
    } else if (i->is_release()) {
      allocator->apply_release(*i);
    } else {
      allocator->note_alloc(*i);
    }
    ++i;
  }
  assert(allocator->initialized());
  in_transaction = false;
  set_Transaction_root(nullptr);
}

int TransactionRoot::start_read_access()
{
  LOCK_READ;
  readers_count++;
  return 0;
}

int TransactionRoot::stop_read_access()
{
  --readers_count;
  UNLOCK_READ;
  return 0;
}

int TransactionRoot::start_transaction()
{
  LOCK;
  assert(idPrev <= idNext);
  assert(((AllocationLog&)alloc_log).committed());
  assert(obj_log.empty());
  set_Transaction_root(this);
  in_transaction = true;
  ++idNext;

  return 0;
}

int TransactionRoot::commit_transaction()
{
  assert(idPrev < idNext);

  if (((AllocationLog&)alloc_log).get_log_size() > alog_squeeze_threshold) {
    std::cerr << "doing log squeeze" << std::endl;
    AllocEntry e = ((AllocationLog&)alloc_log).squeeze(*this, *allocator);

    alloc_log.setup(*this, e);
  }

  // NB: objects2release might grow during the enumeration
  for (size_t pos = 0; pos < objects2release->size(); pos++) {

    auto& d = objects2release->at(pos);
    // d.p is real pointer to PObjBase if destroy_fn != null and persistent mem offset overwise
    if (d.destroy_fn) {
      reinterpret_cast<PObjBase*>(d.p)->destroy(*this, d.len, d.destroy_fn);
    } else {
      free_persistent_raw(reinterpret_cast<uint64_t>(d.p), d.len);
    }
  }
  objects2release->clear();
  in_transaction = false;
  set_Transaction_root(nullptr);

  idPrev.store(idNext);

  // Need to handle in alloc_log reply code the case when we fail exatly at
  // this point. In fact we can just ignore the difference if transaction
  // is not in progress (idPrev == idNext)
  
  ((AllocationLog&)alloc_log).commit();

  // the same handling as above here - ignore the diff if no transaction
  // is in progress
  obj_log.reset();

  UNLOCK;
  return 0;
}

int TransactionRoot::rollback_transaction()
{
  assert(idPrev < idNext);

  objects2release->clear();

  // revert allocations
  auto i = ((AllocationLog&)alloc_log).cur();
  while (i != ((AllocationLog&)alloc_log).end()) {
    if (!i->is_release()) {
      allocator->free(*i);
    }
    ++i;
  }
  ((AllocationLog&)alloc_log).rollback();
  {
    auto i = obj_log.start();
    while (i != obj_log.end()) {
      ObjLogEntry& o = *i;
      poffs2ptr<PObjRecoverable>(o.obj_offs)->recover(o.tid, o.offs);
      ++i;
    }
    obj_log.reset();
  }
  set_Transaction_root(nullptr);

  idNext.store(idPrev);

  UNLOCK;
  return 0;
}

void TransactionRoot::queue_in_progress(
  PObjRecoverable* obj,
  TransactionId tid,
  uint64_t offs)
{
  obj_log.push_back(ObjLogEntry(ptr2poffs(obj), tid, offs)); // FIXME minor: implement as emplace_back?
}

void* PObjBase::operator new(size_t sz, TransactionRoot& tr, size_t dummy)
{
  return reinterpret_cast<void*>(tr.alloc_persistent_raw(sz) + root->base);
}
void PObjBase::operator delete(void* ptr, TransactionRoot& tr, size_t len)
{
  uint64_t offs = reinterpret_cast<uint64_t>(ptr);
  assert(offs > root->base);
  tr.free_persistent_raw(offs - root->base, len);
}
void PObjBase::destroy(TransactionRoot& tr, size_t len, dtor destroy_fn)
{
  destroy_fn(this); // virtual dtor replacement as we can't use virtuals
  operator delete(this, tr, len);
}
