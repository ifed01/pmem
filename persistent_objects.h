#ifndef __PERSISTENT_OBJECTS_H
#define __PERSISTENT_OBJECTS_H

#include <atomic>
#include <vector>
#include <list>
#include <map>
#include <string>
#include <functional>
#include <assert.h>
#include <limits>
#include <shared_mutex>

#include <iostream>

#ifdef __GNUC__
#include <boost/interprocess/offset_ptr.hpp>
using namespace boost::interprocess;
#endif

#include "fastbmap_allocator_impl.h"

namespace PersistentObjects
{
  struct AllocEntry
  {
    uint64_t offset = 0;
    uint32_t length = 0;
    AllocEntry() {}
    AllocEntry(uint64_t o, uint32_t l) : offset(o), length(l) {}
  };
  class TransactionAllocator : public AllocatorLevel02<AllocatorLevel01Loose>
  {
    uint64_t capacity = 0;
  public:
    bool initialized() const {
      return capacity != 0;
    }
    void init(uint64_t size, uint32_t alloc_unit, uint32_t prealloc_size) {
      assert(!initialized()); // duplicate init check
      assert(prealloc_size % alloc_unit == 0);
     _init(size, alloc_unit);
     capacity = size;
     AllocEntry first(0, prealloc_size);
     note_alloc(first);
    }
    void shutdown() {
      capacity = 0;
      _shutdown();
    }

    AllocEntry alloc(size_t uint8_ts);
    uint64_t alloc(size_t uint8_ts, size_t min_size, bufferlist& res);
    void free(const AllocEntry& e);
    void free(const bufferlist& to_release);
    void note_alloc(const AllocEntry& e);
    void apply_release(const AllocEntry& e);

    uint64_t get_capacity() const {
      return capacity;
    }
  };

  typedef uint64_t TransactionId;
  class TransactionRoot;

  typedef void(*dtor)(const void*);

  struct PObjBase
  {
    void* operator new(size_t sz, TransactionRoot& tr, size_t dummy);
    void operator delete(void* p, TransactionRoot&, size_t len);
    void destroy(TransactionRoot& tr, size_t len, dtor destroy_fn);
  };
  struct PObjBaseDestructor
  {
    void* p = nullptr; // this is PObjBase if destroy_fn != null and persisgtent offset overwise
    dtor destroy_fn = nullptr;
    size_t len = 0;
    PObjBaseDestructor(PObjBase* _p, size_t _len, dtor _destroy_fn) :
      p(_p), destroy_fn(_destroy_fn), len(_len) {
      assert(destroy_fn != nullptr);
    }
    PObjBaseDestructor(uint64_t _o, size_t _len) :
      p(reinterpret_cast<void*>(_o)), destroy_fn(nullptr), len(_len) {
    }
  };

  struct PObjRecoverable : public PObjBase
  {
    TransactionId tid = 0;
    uint64_t offs = 0;

    void recover(TransactionId _tid, uint64_t _offs)
    {
      tid = _tid;
      offs = _offs;
    }
  };

  const uint64_t MIN_OBJECT_SIZE = sizeof(PObjRecoverable);
  const size_t ALLOC_SNAPSHOT_PAGE = 4096;
  const uint32_t TR_ROOT_PREALLOC_SIZE = 64 * 1024;
  /*template <class T>
  T* alloc_persistent(size_t size = 1) {
    return new T[size];
  } */

  extern thread_local TransactionRoot* working_Transaction_root;
  // this should be applied every time we start processing transaction within
  // a specific thread
  void set_Transaction_root(TransactionRoot* tr);

  struct PersistencyRoot
  {
    uint64_t runId = 1;
    uint64_t base = 0;
    void init()
    {
      // FIXME: this should be mapped to mmap result?
      base = 0;
    }
    void restart()
    {
      // FIXME: this should be mapped to mmap result?
      //base = 0;
      ++runId;
    }
  };

  // FIXME: in fact we need to obtain that root from persistent store(pool)
  extern PersistencyRoot* root;
  template <class T>
  uint64_t ptr2poffs(T* ptr) {
    uint64_t offs = reinterpret_cast<uint64_t>(ptr);
    assert(offs > root->base);
    offs -= root->base;
    return offs;
  }
  template <class T>
  T* poffs2ptr(uint64_t offs) {
    return reinterpret_cast<T*>(offs + root->
      base);
  }
  template <class T>
  class VPtr
  {
    uint64_t runId = 0;
    T* ptr = nullptr;
  public:
    VPtr() {}
    VPtr(T* p) : runId(root->runId), ptr(p)
    {
    }
    VPtr(const VPtr& from)
    {
      if (from.runId == root->runId) {
        runId = from.runId;
        ptr = from.ptr;
      }
    }
    // for the sake of feature parity with PPtr
    inline bool is_null() const {
      return (runId != root->runId || ptr == nullptr);
    }
    operator T*()
    {
      if (runId == root->runId) {
        return ptr;
      }
      return nullptr;
    }
    operator const T*() const
    {
      if (runId == root->runId) {
        return ptr;
      }
      return nullptr;
    }

    T* operator ->()
    {
      if (runId == root->runId) {
        return ptr;
      }
      return nullptr;
    }
    const T* operator ->() const
    {
      if (runId == root->runId) {
        return ptr;
      }
      return nullptr;
    }
    void reset(T* p)
    {
      runId = root->runId;
      ptr = p;
    }
  };

  class PBuffer : public PObjRecoverable
  {
    size_t len = 0;
  public:
    PBuffer() {
    }
    ~PBuffer() {
    }
    bool is_null() const {
      return offs == 0;
    }
    //non-Transaction set
    void setup_initial(TransactionId _tid, uint64_t _offs, size_t _len) {
      tid = _tid;
      offs = _offs;
      len = _len;
    }
    void setup_new(TransactionRoot& t, uint64_t offs, size_t new_size);
    inline void* get() const {
      assert(tid != 0 && offs);
      return poffs2ptr<void*>(offs);
    }
    inline const uint8_t* inspect() const {
      assert(tid != 0 && offs);
      return poffs2ptr<uint8_t>(offs);
    }
    inline void die(TransactionRoot& t);
  };
  template <class T>
  class PObj : public PObjRecoverable
  {
  protected:
    inline T* _get() const {
      assert(tid != 0 && offs);
      return reinterpret_cast<T*>(offs + root->base);
    }
  public:
    PObj(nullptr_t)
    {
    }
    PObj(TransactionRoot& t);
    PObj(TransactionId _tid, T* _ptr)
    {
      tid = _tid;
      offs = ptr2poffs<T>(_ptr);
    }
    inline const T* operator->() const {
      return _get();
    }
    operator const T&() const
    {
      return *_get()();
    }

    inline const T* inspect() const {
      return _get();
    }
    inline T* access(TransactionRoot& t);
    bool is_null() const {
      return offs == 0;
    }
    inline void die(TransactionRoot& t);
  };
  template <class T>
  class PUniquePtr : public PObj<T>
  {
    size_t len = 0;
  public:
    PUniquePtr() : PObj<T>(nullptr)
    {
    }
    PUniquePtr(nullptr_t) : PObj<T>(nullptr)
    {
    }
    //non-Transaction set
    void setup_initial(TransactionId _tid, uint64_t _offs, size_t _len = 0) {
      PObj<T>::tid = _tid;
      PObj<T>::offs = _offs;
      len = _len;
    }

    inline void setup(TransactionRoot& t, const AllocEntry& a);
    
    template <typename... Args>
    void allocate_obj(TransactionRoot& tr, Args&&... args);
    inline void die(TransactionRoot& t);
/*    T* operator->() {
      return _get();
    }*/
    operator T&() {
      return *PObj<T>::_get();
    }
    operator const T&() const {
      return *PObj<T>::_get();
    }

    // a doubtful hack to provide non-const access
    // intended to permit modification (i.e. access call) without 
    // "accessing" the owner
    PUniquePtr<T>* operator->() const {
      return const_cast<PUniquePtr<T>*>(this);
    }
  };

  class TransactionRoot
  {
    std::atomic<TransactionId> idPrev = { 0 };
    std::atomic<TransactionId> idNext = { 0 };

    struct AllocLogEntry : public AllocEntry
    {
      enum {
        RELEASE_FLAG = 1,
        INIT_FLAG = 2,
      };
      uint32_t flags;
      inline bool is_release() const {
        return flags & RELEASE_FLAG;
      }
      inline bool is_init() const {
        return flags & INIT_FLAG;
      }
      void set(const AllocEntry& e, uint32_t _flags) {
        offset = e.offset;
        length = e.length;
        flags = _flags;
      }
      void set(uint64_t offs, uint32_t len, uint32_t _flags) {
        offset = offs;
        length = len;
        flags = _flags;
      }
    };
    template <class Log, class Entry>
    class LogIteratorProto
    {
      size_t pos = 0;
      Log& log;
    public:
      LogIteratorProto(Log& _log, size_t _pos) : log(_log), pos(_pos) {
      }
      Entry* operator->() {
        return &log.at(pos);
      }
      Entry& operator*() {
        return log.at(pos);
      }
      bool operator!=(const LogIteratorProto& other) const {
        return pos != other.pos;
      }
      void operator++() {
        pos++;
      }
    };

    class AllocationLog : public PObjBase {
      size_t alloc_log_size = 0;
      size_t alloc_log_start = 0; // needful?
      size_t alloc_log_cur = 0;
      size_t alloc_log_next = 0;
      size_t alloc_log_base_cnt = 0;
      uint64_t snapshot_alloc_cnt = 0;
      size_t snapshot_blist_size = 0;
      PBuffer snapshot_bufferlist;
      AllocLogEntry log[1];
    public:
      typedef LogIteratorProto<AllocationLog, AllocLogEntry> Iterator;
      Iterator start() {
        return Iterator(*this, alloc_log_start);
      }
      Iterator cur() {
        return Iterator(*this, alloc_log_cur);
      }
      Iterator end() {
        return Iterator(*this, alloc_log_next);
      }
      static AllocEntry create_new(TransactionId tid,
                                        TransactionAllocator& alloc,
                                        const AllocLogEntry& first,
                                        size_t log_size) {
        auto alloc_cnt0 = alloc.get_alloc_count();
        // NB: adjust by - 1 as sizeof(AllocationLog) takes one into account
        auto buf_size = (log_size - 1) * sizeof(AllocLogEntry);
        buf_size += sizeof(AllocationLog);
        AllocEntry self = alloc.alloc(buf_size);
        assert(self.length >= buf_size);
        AllocationLog* alog = poffs2ptr<AllocationLog>(self.offset);

        alog->alloc_log_size = log_size;
        alog->alloc_log_start = alog->alloc_log_cur = alog->alloc_log_next = 0;
        alog->alloc_log_base_cnt = alloc.get_alloc_count() - alloc_cnt0;
        alog->snapshot_blist_size = 0;
        alog->snapshot_alloc_cnt = 0;
        alog->snapshot_bufferlist.setup_initial(tid, 0, 0);
        alog->next() = first;
        alog->next().set(self, 0);
        alog->commit();
        return self;
      }
      void die(TransactionRoot& t);

      void apply_allocator_snapshot(TransactionAllocator& alloc);

      AllocEntry squeeze(TransactionRoot& t, TransactionAllocator& alloc);
      AllocLogEntry& at(size_t i) {
        assert(i < alloc_log_next);
        return *(log + i);
      }
      AllocLogEntry& next() {
        return log[alloc_log_next++];
      }
      bool committed() const {
        return alloc_log_cur == alloc_log_next;
      }
      void commit() {
        alloc_log_cur = alloc_log_next;
      }
      void rollback() {
        alloc_log_next = alloc_log_cur;
      }
      size_t get_log_size() const {
        return alloc_log_next - alloc_log_start;
      }
      size_t get_base_cnt() const {
        return alloc_log_base_cnt;
      }
    };
    PUniquePtr <AllocationLog> alloc_log;
    size_t alloc_base_cnt = 0;
    size_t alog_squeeze_threshold = 0;
    VPtr<TransactionAllocator> allocator;

    std::atomic<int> readers_count; // debug only
    bool in_transaction = false; // debug only

    struct ObjLogEntry
    {
      uint64_t obj_offs = 0;
      TransactionId tid = 0;
      uint64_t offs = 0;
      ObjLogEntry() {}
      ObjLogEntry(uint64_t _obj_offs, TransactionId _tid, uint64_t _offs)
        : obj_offs(_obj_offs), tid(_tid), offs(_offs) {}
    };

    class ObjectLog {
      PBuffer buf;
      size_t obj_log_size = 0;
      size_t obj_log_base_cnt = 0;
      size_t obj_log_start = 0; // needful?
      size_t obj_log_end = 0;

    public:
      typedef LogIteratorProto<ObjectLog, ObjLogEntry> Iterator;
      Iterator start() {
        return Iterator(*this, obj_log_start);
      }
      Iterator end() {
        return Iterator(*this, obj_log_end);
      }
      void prepare(TransactionId tid,
        TransactionAllocator& alloc,
        AllocationLog& alog,
        size_t log_size) {
        assert(obj_log_size == 0);
        auto alloc_cnt0 = alloc.get_alloc_count();
        auto buf_size = log_size * sizeof(ObjLogEntry);
        AllocEntry self = alloc.alloc(buf_size);
        assert(self.length == buf_size);
        buf.setup_initial(tid,
          self.offset,
          self.length);
        obj_log_size = log_size;
        obj_log_start = obj_log_end = 0;
        obj_log_base_cnt = alloc.get_alloc_count() - alloc_cnt0;
        auto& alog_entry = alog.next();
        alog_entry.set(self, 0);
        alog.commit();
      }
      ObjLogEntry& at(size_t i) const {
        assert(i < obj_log_end);
        return (reinterpret_cast<ObjLogEntry*>(buf.get()))[i];
      }
      void push_back(const ObjLogEntry& e) {
        assert(obj_log_end < obj_log_size);
        (reinterpret_cast<ObjLogEntry*>(buf.get()))[obj_log_end] = e;
        ++obj_log_end;
      }
      bool empty() const {
        return obj_log_start == obj_log_end;
      }
      void reset() {
        obj_log_end = obj_log_start;
      }
      uint64_t get_base_cnt() const {
        return obj_log_base_cnt;
      }
    } obj_log;

    std::vector<PObjBaseDestructor>* objects2release = nullptr;
    std::shared_mutex* lock = nullptr;

#define LOCK lock->lock()
#define UNLOCK lock->unlock();
#define LOCK_READ lock->lock_shared();
#define UNLOCK_READ lock->unlock_shared();

  public:

    ~TransactionRoot()
    {
      assert(!in_transaction);
      //FIXME: different implementation when root is persistent?
      //free((void*)root->base);
      //root->base = 0;
      delete objects2release;
      delete lock;
    }

    static TransactionRoot* create(uint64_t capacity)
    {
      assert(root->base == 0);
      root->base = (uint64_t)malloc(capacity); // FIXME: access and allocate PMem. Do mmap?
      assert(root->base != 0);
      TransactionRoot* res = new ((void*)root->base) TransactionRoot;
      return res;
    }
    static void destroy(TransactionRoot* tr)
    {
      assert(root->base != 0);
      tr->~TransactionRoot();
      free((void*)root->base);
      root->base = 0;
    }

    void prepare(size_t _alloc_log_size,
      size_t _alog_squeeze_threshold,
      size_t _obj_log_size,
      uint64_t capacity,
      uint32_t min_alloc_unit)
    {
      assert(idNext == 0);
      assert(idNext == idPrev);
      idNext = idPrev = 1;

      objects2release = new std::remove_pointer<decltype(objects2release)>::type;
      lock = new std::shared_mutex();

      allocator = new TransactionAllocator();
      allocator->init(capacity, min_alloc_unit, TR_ROOT_PREALLOC_SIZE);
      alloc_base_cnt = allocator->get_alloc_count();
      alog_squeeze_threshold = _alog_squeeze_threshold;

      AllocLogEntry first;
      first.flags = AllocLogEntry::INIT_FLAG;
      first.offset = capacity;
      first.length = min_alloc_unit;
      //auto allocs0 = allocator.get_alloc_count();
      AllocEntry alog_entry = AllocationLog::create_new(
        idNext,
        *allocator,
        first,
        _alloc_log_size);
      alloc_log.setup_initial(idNext, alog_entry.offset, alog_entry.length);
      obj_log.prepare(idNext, *allocator, alloc_log, _obj_log_size);
    }
    void shutdown() {
      // reset volatile members, assuming they might exist, e.g. if we simulate restart
      delete objects2release;
      objects2release = nullptr;
      delete lock;
      lock = nullptr;
      if (allocator) {
        allocator->shutdown();
        delete (TransactionAllocator*)allocator;
        allocator = nullptr;
      }
    }
    // indicates instance restart
    void restart()
    {

      //FIXME: we'll need to reset root base here once real PM is used 
      /*if (root->base != 0) {
        free((void*)root->base);
        root->base = 0;
      } */
      shutdown();
      objects2release = new std::remove_pointer<decltype(objects2release)>::type;
      lock = new std::shared_mutex();
      allocator = new TransactionAllocator;

      replay();

      assert(root->base != 0);
    }

    inline TransactionId get_effective_id() const {
      return idNext;
    }
    inline TransactionId get_stable_id() const {
      return idPrev;
    }
    void replay();

    uint64_t alloc_persistent_raw(size_t uint8_ts)
    {
      // permit within transaction scope only
      assert(in_transaction);
      AllocLogEntry& e = ((AllocationLog&)alloc_log).next();
      e.set(allocator->alloc(uint8_ts), 0);
      return e.offset;
    }
    void free_persistent_raw(uint64_t offs, size_t len)
    {
      // permit within transaction scope only
      assert(in_transaction);
      AllocLogEntry& e = ((AllocationLog&)alloc_log).next();
      e.set(offs,
            (uint32_t)len,
            AllocLogEntry::RELEASE_FLAG);
      allocator->free(e);
    }

    int start_read_access();
    int stop_read_access();

    int start_transaction();

    int commit_transaction();
    int rollback_transaction();

    void queue_for_release(PObjBase* t, size_t len, dtor destroy_fn)
    {
      objects2release->emplace_back(t, len, destroy_fn);
    }
    void queue_for_release(uint64_t offs, size_t len)
    {
      objects2release->emplace_back(offs, len);
    }
    void queue_in_progress(PObjRecoverable* obj, TransactionId tid, uint64_t offs);
    size_t get_object_count()
    {
      return allocator->get_alloc_count() -
        ((const AllocationLog&)alloc_log).get_base_cnt() -
        obj_log.get_base_cnt() - 
        alloc_base_cnt;
    }
    uint64_t get_available() {
      return allocator->get_available();
    }
    size_t get_alog_size() const {
      return ((const AllocationLog&)alloc_log).get_log_size();
    }
  };

  template <class T>
  PObj<T>::PObj(TransactionRoot& t)
  {
    tid = t.get_effective_id();
    void* ptr = new (t) T();
    offs = ptr2poffs<T>(ptr);
  }

  template <class T>
  T* PObj<T>::access(TransactionRoot& t) {
    assert(tid != 0 && offs);
    auto _tid = t.get_effective_id();
    if (_tid == tid)
      return _get();

    // duplicate
    t.queue_in_progress(this, tid, offs);
    t.queue_for_release(_get(), sizeof(T), [](const void* x) {
      static_cast<const T*>(x)->~T(); });

    tid = _tid;
    T* ptr = new (t, 0) T(*_get());
    offs = ptr2poffs<T>(ptr);
    return ptr;
  }

  template <class T>
  inline void PObj<T>::die(TransactionRoot& t) {
    assert(offs);
    t.queue_in_progress(this, tid, offs);
    t.queue_for_release(this,
      sizeof(*this), [](const void* x) {
      static_cast<const PObj<T>*>(x)->~PObj<T>(); });
    t.queue_for_release(_get(), sizeof(T), [](const void* x) {
      static_cast<const T*>(x)->~T(); });

    // dtor simulation.
    // we can probably get rid off it by enforcing ref_counted ptr usage
    // inside persisent objects
    _get()->die(t);

    tid = 0;
    offs = 0;
  }
 
  template <class T>
  void PUniquePtr<T>::setup(TransactionRoot& t, const AllocEntry& a) {
    auto _tid = t.get_effective_id();
    assert(_tid != PObj<T>::tid);

    t.queue_in_progress(this, PObj<T>::tid, PObj<T>::offs);
    if (!PObj<T>::is_null()) {
      t.queue_for_release(PObj<T>::_get(), len ? len : sizeof(T), [](const void* x) {
        static_cast<const T*>(x)->~T(); });

      PObj<T>::_get()->die(t);
    }

    PObj<T>::tid = _tid;
    PObj<T>::offs = a.offset;
    len = a.length;

    return;
  }

  template <class T>
  template <typename... Args>
  void PUniquePtr<T>::allocate_obj(TransactionRoot& tr, Args&&... args) {
    T* t = new (tr, 0) T(args...);
    PObj<T>::tid = tr.get_effective_id();
    PObj<T>::offs = ptr2poffs(t);
  }
  template <class T>
  void PUniquePtr<T>::die(TransactionRoot& t) {
    if (!PObj<T>::offs) {
      return;
    }
    t.queue_in_progress(this, PObj<T>::tid, PObj<T>::offs);
    t.queue_for_release(PObj<T>::_get(), len ? len : sizeof(T), [](const void* x) {
      static_cast<const T*>(x)->~T(); });

    // dtor simulation.
    // we can probably get rid off it by enforcing ref_counted ptr usage
    // inside persisent objects
    PObj<T>::_get()->die(t);

    PObj<T>::tid = 0;
    PObj<T>::offs = 0;
  }

  template <class T>
  class PPtrRootOffset
  {
    uint64_t offs = 0;

    PPtrRootOffset(uint64_t _offs)
    {
      offs = _offs;
    }

  public:

    typedef T              element_type;
    typedef T              value_type;

    template <class U>
    using rebind = PPtrRootOffset<U>;

    PPtrRootOffset()
    {
    }
    PPtrRootOffset(nullptr_t)
    {
    }
    PPtrRootOffset(T* p)
    {
      if (p == nullptr) {
        offs = 0;
      } else {
        offs = ptr2poffs(p);
      }
    }

    PPtrRootOffset(const PPtrRootOffset& from)
    {
      offs = from.offs;
    }
    inline bool is_null() const {
      return offs == 0;
    }
    PPtrRootOffset<T> operator++()
    {
      offs += sizeof(T);
      return this;
    }
    T* get() const {
      return poffs2ptr<T>(offs);
    }
    operator T*() const {
      return get();
    }
    T* operator ->() const {
      return get();
    }
    inline void die(TransactionRoot& t) {
      get()->die(t);
    }
    size_t operator -(const PPtrRootOffset<T>& p) const
    {
      return (offs - p.offs) / sizeof(T);

    }
    PPtrRootOffset<T>& operator +(size_t delta) const
    {
      PPtrRootOffset<T> p(offs + delta * sizeof(T));
      return p;
    }

    bool operator !=(const PPtrRootOffset<T>& p) const
    {
      return offs != p.offs;
    }
    bool operator !=(nullptr_t) const
    {
      return offs != 0;
    }
    template <typename U>
    static PPtrRootOffset<T> pointer_to(U& r)
    {
      return PPtrRootOffset<T>(&r);
    }

    template <class U, typename... Args>
    static PPtrRootOffset<PObj<U>> alloc_persistent_obj(TransactionRoot& tr, Args&&... args) {
      U* t = new (tr, 0) U(args...);
      return new (tr, 0) PObj<U>(tr.get_effective_id(), t);
    }
  };

  //This is a sort of offset_ptr implementation but it doesn't work under MSVC++ (but is fineunder gcc)
  //Looks like a Microsoft stl implementation bug, reverting to previous Ptr for now
  template <class T>
  class PPtrThisOffset
  {
    bool was_nullptr = false;
    int64_t offset = 0;
    inline void assign(T* p)
    {
//      std::cout << " Assign " << p << " to " << this << std::endl;
      if (p == nullptr) {
        offset = 0;
        was_nullptr = true;
      } else {
        // check if we do not overflow int64_t
        assert((uint8_t*)p >= (uint8_t*)this ?
          (uint8_t*)p <= (uint8_t*)this + std::numeric_limits<int64_t>::max() :
          (uint8_t*)this <= (uint8_t*)p + std::numeric_limits<int64_t>::max());
        offset = (uint8_t*)p - (uint8_t*)this;
        was_nullptr = false;
      }
    }
  public:

    typedef T              element_type;
    typedef T              value_type;

    PPtrThisOffset()
    {
    }
    PPtrThisOffset(nullptr_t)
    {
      assign(nullptr);
    }
    PPtrThisOffset(T* p)
    {
      assign(p);
    }

    PPtrThisOffset(const PPtrThisOffset& from)
    {
      assign((T*)from);
    }
    PPtrThisOffset<T> operator=(const PPtrThisOffset& from)
    {
      assign((T*)from);
      return *this;
    }
    PPtrThisOffset<T> operator=(nullptr_t)
    {
      assign(nullptr);
      return *this;
    }
    PPtrThisOffset<T> operator++()
    {
      //assert(offset != 0);
      assert(offset < 0 ||
        (uint64_t)offset + sizeof(T) <= std::numeric_limits<int64_t>::max());
      offset += sizeof(T);
      return this;
    }
    operator T*() const
    {
      //return reinterpret_cast<T*>( offset ? (uint8_t*)this + offset : nullptr);
      T* p = reinterpret_cast<T*>(!was_nullptr/*offset*/ ? (uint8_t*)this + offset : nullptr);
//      std::cout << " Ret " << p << " for " << this << std::endl;
      return p;

    }

    T* operator ->() const
    {
      T* p = reinterpret_cast<T*>(!was_nullptr/*offset*/ ? (uint8_t*)this + offset : nullptr);
      return p;
    }
    int64_t operator -(const PPtrThisOffset<T>& p) const
    {
      return (T*)(*this) - p;
    }

    PPtrThisOffset<T> operator +(size_t delta) const
    {
      PPtrThisOffset<T> p( (T*)(*this) + delta);
      return p;
    }

    bool operator !=(const PPtrThisOffset<T>& p) const
    {
      return (T*)(*this) != p;
    }
    bool operator !=(nullptr_t) const
    {
      //return offset != 0;
      return !was_nullptr;
    }
    template <typename U>
    static PPtrThisOffset<T> pointer_to(U& r)
    {
      return PPtrThisOffset<T>(&r);
    }

    template <class U, typename... Args>
    static PPtrThisOffset<PObj<U>> alloc_persistent_obj(TransactionRoot& tr, Args&&... args) {
      U* t = new (tr) U(args...);
      return new (tr) PObj<U>(tr.get_effective_id(), t);
    }
  };

#ifdef __GNUC__
  template <class T>
  class PBoostOffsetPtr : public offset_ptr<T>
  {
  public:
    template <class U>
    using rebind = PBoostOffsetPtr<U>;

    PBoostOffsetPtr() : offset_ptr<T>()
    {
    }
    PBoostOffsetPtr(nullptr_t) : offset_ptr<T>(nullptr)
    {
    }
    PBoostOffsetPtr(typename offset_ptr<T>::pointer p) : offset_ptr<T>(p)
    {
    }
    PBoostOffsetPtr(const PBoostOffsetPtr& from) : offset_ptr<T>(from)
    {
    }
    // ctor to cast ptr<T> to ptr<const T>
    // may be use rebind somehow for that?
    template <class U>
    PBoostOffsetPtr(const PBoostOffsetPtr<U>& from)
      : offset_ptr<T>(static_cast<const T*>(from))
    {
      //std::enable_if<std::is_same<const T, const U>::value>::type;
    }
    inline bool is_null() const {
      return offset_ptr<T>::get() == nullptr;
    }
    inline void die(TransactionRoot& t) {
      offset_ptr<T>::get()->die(t);
    }
    inline PBoostOffsetPtr& operator=(const PBoostOffsetPtr from)
    {
      offset_ptr<T>::operator=(from);
      return *this;
    }

    inline operator T*() const
    {
      return offset_ptr<T>::get();
    }

    using diff_type = typename offset_ptr<T>::difference_type;
    inline PBoostOffsetPtr& operator+=(diff_type d) noexcept
    {
      offset_ptr<T>::operator+=(d);
      return *this;
    }
    inline PBoostOffsetPtr& operator-=(diff_type d) noexcept
    {
      offset_ptr<T>::operator-=(d);
      return *this;
    }
    inline PBoostOffsetPtr& operator++(void) noexcept
    {
      offset_ptr<T>::operator++();
      return *this;
    }
    inline PBoostOffsetPtr operator++(int) noexcept
    {
      return offset_ptr<T>::operator++((int)1);
    }
    inline PBoostOffsetPtr& operator--(void) noexcept
    {
      offset_ptr<T>::operator--();
      return *this;
    }
    inline PBoostOffsetPtr operator--(int) noexcept
    {
      return offset_ptr<T>::operator--((int)1);
    }

    // friend functions
    friend PBoostOffsetPtr<T> operator+(diff_type d, PBoostOffsetPtr<T> other) noexcept
    {
      other += d;
      return other;
    }
    friend PBoostOffsetPtr<T> operator+(PBoostOffsetPtr<T> other, diff_type d) noexcept
    {
      other += d;
      return other;
    }
    friend PBoostOffsetPtr<T> operator-(PBoostOffsetPtr<T> other, diff_type d) noexcept
    {
      other -= d;
      return other;
    }
    /*friend PBoostOffsetPtr<T> operator-(diff_type d, PBoostOffsetPtr<T> other) noexcept
    {
      FIXME: wrong?
      other -= d;
      return other;
    }*/

    template <class U, typename... Args>
    static PBoostOffsetPtr<PObj<U>> alloc_persistent_obj(TransactionRoot& tr, Args&&... args) {
      U* t = new (tr, 0) U(args...);
      return new (tr, 0) PObj<U>(tr.get_effective_id(), t);
    }
  };
  template <typename T>
  using PPtr = PBoostOffsetPtr<T>;
  //using PPtr = PPtrRootOffset<T>;
#else
  template <typename T>
  using PPtr = PPtrRootOffset<T>;
//  using PPtr = PPtrThisOffset<T>;
#endif

  template <class T>
  struct persistent_allocator2
  {
#define ALLOCATOR_TRAITS2(T)                \
  typedef persistent_allocator2<T> allocator_type;            \
  typedef T                      type;            \
  typedef type                   value_type;      \
  typedef PPtr<value_type>       pointer;         \
  typedef PPtr<const value_type> const_pointer;   \
  typedef value_type&            reference;       \
  typedef value_type const&      const_reference; \
  typedef std::size_t            size_type;       \
  typedef PPtr<void>             void_pointer;  \
  typedef PPtr<const void>       const_void_pointer; \
  typedef std::ptrdiff_t         difference_type;

    ALLOCATOR_TRAITS2(T);

    template<typename U>
    struct rebind
    {
      typedef persistent_allocator2<U> other;
    };

    // Default Constructor
    persistent_allocator2(void) {}

    // Copy Constructor
    template<typename U>
    persistent_allocator2(persistent_allocator2<U> const& other) {}

    // Allocate memory
    pointer allocate(size_type count, const_pointer /* hint */ = nullptr)
    {
      //if (count > max_size()) { throw std::bad_alloc(); }
      assert(working_Transaction_root);
      //return static_cast<pointer>(working_Transaction_root->alloc_persistent_raw(count * sizeof(type)));
      //pointer p;
      //p.reset(working_Transaction_root->alloc_persistent_raw(count * sizeof(type)));
      //return p;

      //return pointer::alloc_persistent_count(*working_Transaction_root, count);

      uint64_t* p = reinterpret_cast<uint64_t*>(
        working_Transaction_root->alloc_persistent_raw(count * sizeof(type) + sizeof(uint64_t)) +
          root->base);
      *p = count * sizeof(type) + sizeof(uint64_t);
      return pointer((type*)(p + 1));
    }

    // Delete memory
    void deallocate(pointer ptr, size_type /* count */)
    {
      if (ptr.is_null())
        return;
      //std::cout << "deallocate"<< (uint64_t)(T*)ptr << std::endl;
      assert(working_Transaction_root);
      uint64_t* p = (uint64_t*)(void*)ptr;
      --p;
      uint64_t offs = reinterpret_cast<uint64_t>(p);
      assert(offs > root->base);
      offs -= root->base;
      working_Transaction_root->queue_for_release(offs, *p);
    }
/*    template <class... Args>
    static void construct (allocator_type& alloc, pointer p, Args&&... args)
    {
      ::new ((void*)p.inspect()) typename pointer::PointerType (std::forward<Args>(args)...);
    }
    template <class... Args>
    static void construct (allocator_type& alloc, T* p, Args&&... args)
    {
      //typename pointer::PointerType pppp;
      ::new ((void*)p) typename pointer::PointerType (std::forward<Args>(args)...);
    }
    template <class... Args>
    void construct (pointer p, Args&&... args)
    {
      ::new ((void*)p.inspect()) T (std::forward<Args>(args)...);
    }
    template <class U, class... Args>
    static void construct (persistent_allocator2<U> alloc, PPtr<U> p, Args&&... args)
    {
      ::new ((void*)p.inspect()) U (std::forward<Args>(args)...);
    }
    template <class U, class... Args>
    static void construct (persistent_allocator2<U> alloc, U* p, Args&&... args)
    {
      ::new ((void*)p) U (std::forward<Args>(args)...);
    }

    template <class U, class... Args>
    void construct (U* p, Args&&... args)
    {
      ::new ((void*)p) U (std::forward<Args>(args)...);
    }
    template <class U, class... Args>
    void construct (PPtr<U> p, Args&&... args)
    {
      ::new ((void*)p.inspect()) U (std::forward<Args>(args)...);
    }*/
    template <class... Args>
    void construct (T* p, Args&&... args)
    {
      ::new ((void*)p) T (std::forward<Args>(args)...);
    }
    void destroy (T* p)
    {
      //std::cout << "destroy " << (uint64_t)(void*)p << std::endl;
      p->~T();
    }
    inline bool operator==(persistent_allocator2 const&) { return true; }
    inline bool operator!=(persistent_allocator2 const& a) { return !operator==(a); }
  };

#ifndef __GNUC__
  // FIXME: workaround for VC++. 
  // persistent_vector is not functional in real world!!!, 
  // just to be able to develop using this way.
  template <class T>
  struct persistent_allocator
  {
#define ALLOCATOR_TRAITS(T)                \
  typedef T                 type;            \
  typedef type              value_type;      \
  typedef value_type*       pointer;         \
  typedef value_type const* const_pointer;   \
  typedef value_type&       reference;       \
  typedef value_type const& const_reference; \
  typedef std::size_t       size_type;       \
  typedef std::ptrdiff_t    difference_type;

    ALLOCATOR_TRAITS(T);

    template<typename U>
    struct rebind
    {
      typedef persistent_allocator<U> other;
    };

    // Default Constructor
    persistent_allocator(void) {}

    // Copy Constructor
    template<typename U>
    persistent_allocator(persistent_allocator<U> const& other) {}

    // Allocate memory
    pointer allocate(size_type count, const_pointer /* hint */ = 0)
    {
      //if (count > max_size()) { throw std::bad_alloc(); }
      assert(working_Transaction_root);
      uint64_t* p = reinterpret_cast<uint64_t*>(
        working_Transaction_root->alloc_persistent_raw(count * sizeof(type) + sizeof(uint64_t)) +
        root->base);
      *p = count * sizeof(type) + sizeof(uint64_t);
      return pointer((void*)(p+1));
    }

    // Delete memory
    void deallocate(pointer ptr, size_type /* count */)
    {
      if (ptr == nullptr)
        return;
      assert(working_Transaction_root);
      uint64_t* p = (uint64_t*)(void*)ptr;
      --p;

      uint64_t offs = reinterpret_cast<uint64_t>(p);
      assert(offs > root->base);
      offs -= root->base;
      working_Transaction_root->queue_for_release(offs, *p);
    }
  };
  template <class T>
  class persistent_vector : public std::vector<T, persistent_allocator<T>>
  {};
  template <typename T>
  using persistent_vector2 = persistent_vector<T>;
#else
  template <class T>
  using persistent_vector2 = std::vector<T, persistent_allocator2<T>>;
#endif

  template <class T>
  using persistent_list2 = std::list<T, persistent_allocator2<T>>;

  template <class K, class V>
  using persistent_map2 = std::map<K, V, std::less<K>, persistent_allocator2<std::pair<const K, V>>>;
}; // namespace PersistentObjects

#define PERSISTENT_CLASS_DECL(c) \
  class c; \
  typedef PersistentObjects::PPtr<PersistentObjects::PObj<c>> c##Ptr; \
  typedef PersistentObjects::PUniquePtr<c> c##UniquePtr;

#define PERSISTENT_CLASS(c) \
  PERSISTENT_CLASS_DECL(c)  \
  class c : public PersistentObjects::PObjBase

#define PERSISTENT_DEAD void die(TransactionRoot& tr)

#define PERSISTENT_DIE(a) { \
  if (!a.is_null()) {\
    a.die(tr);\
  }\
}
#endif
