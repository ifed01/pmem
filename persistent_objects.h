#ifndef __PERSISTENT_OBJECTS_H
#define __PERSISTENT_OBJECTS_H

#include <atomic>
#include <vector>
#include <list>
#include <string>
#include <functional>
#include <assert.h>

namespace PersistentObjects
{
  struct AllocEntry
  {
    uint64_t offset = 0;
    uint32_t length = 0;
    AllocEntry() {}
    AllocEntry(uint64_t o, uint32_t l) : offset(o), length(l) {}
  };
  class TransactionalAllocator
  {
  public:
    static std::atomic<int> alloc_cnt;
    AllocEntry alloc(size_t uint8_ts);
    void free(const AllocEntry& e);
    void note_alloc(const AllocEntry& e)
    {
    }
    void apply_release(const AllocEntry& e)
    {
      // FIXME
    }
  };

  typedef uint64_t TransactionId;
  class TransactionalRoot;

  typedef void(*dtor)(const void*);

  struct PObjBase
  {
    void* operator new(size_t sz, TransactionalRoot& tr);
    void operator delete(void* p, TransactionalRoot&);
    void destroy(TransactionalRoot& tr, dtor destroy_fn);
  };
  struct PObjBaseDestructor
  {
    PObjBase* p = nullptr;
    dtor destroy_fn = nullptr;
    PObjBaseDestructor(PObjBase* _p, dtor _destroy_fn) :
      p(_p), destroy_fn(_destroy_fn) {
    }
  };

  struct PObjRecoverable : public PObjBase
  {
    TransactionId tid = 0;
    void* ptr = nullptr;

    void recover(TransactionId _tid, void* _ptr)
    {
      tid = _tid;
      ptr = _ptr;
    }
  };

  template <class T>
  T* alloc_persistent(size_t size = 1) {
    return new T[size];
  }

  extern thread_local TransactionalRoot* working_transactional_root;
  // this should be applied every time we start processing transaction within
  // a specific thread
  void set_transactional_root(TransactionalRoot* tr);

  struct PersistencyRoot
  {
    uint64_t runId = 1;
    uint8_t base = 0;
    void init()
    {
      // FIXME: this should be mapped to mmap result?
      base = 0;
    }
    void restart()
    {
      // FIXME: this should be mapped to mmap result?
      base = 0;
      ++runId;
    }
  };

  // FIXME: in fact we need to obtain that root from persistent store(pool)
  extern PersistencyRoot* root;

  class TransactionalRoot
  {
    std::atomic<TransactionId> idPrev;
    std::atomic<TransactionId> idNext;

    struct AllocLogEntry : public AllocEntry
    {
      enum {
        RELEASE_FLAG = 1,
      };
      uint32_t flags;
      inline bool is_release() const {
        return flags & RELEASE_FLAG;
      }
    };

    size_t alloc_log_size = 0;
    AllocLogEntry* alloc_log = nullptr;
    size_t alloc_log_start = 0;
    size_t alloc_log_cur = 0;
    size_t alloc_log_next = 0;
    TransactionalAllocator allocator;

    struct ObjLogEntry
    {
      PObjRecoverable* obj = nullptr;
      TransactionId tid = 0;
      void* ptr = nullptr;
      ObjLogEntry() {}
      ObjLogEntry(PObjRecoverable* _obj, TransactionId _tid, void* _ptr)
        : obj(_obj), tid(_tid), ptr(_ptr) {}
    };
    ObjLogEntry* obj_log = nullptr;
    size_t obj_log_size = 0;
    size_t obj_log_start = 0;
    size_t obj_log_end = 0;

    std::vector<PObjBaseDestructor>* objects2release = nullptr;

    // FIXME
#define LOCK
#define UNLOCK

  public:

    //FIXME: minor - different log sizes?
    TransactionalRoot(size_t _log_size) : idNext(1), idPrev(1),
      alloc_log_size(_log_size), obj_log_size(_log_size)
    {
      alloc_log = alloc_persistent<AllocLogEntry>(_log_size);
      obj_log = alloc_persistent<ObjLogEntry>(_log_size);
    }
    // indicates instance restart
    void restart()
    {
      // reset volatile members
      objects2release = new std::remove_pointer<decltype(objects2release)>::type;

      replay();
    }

    inline TransactionId get_effective_id() const {
      return idNext;
    }
    inline TransactionId get_stable_id() const {
      return idPrev;
    }
    void replay();

    void* alloc_persistent_raw(size_t uint8_ts)
    {
      AllocLogEntry& e = alloc_log[alloc_log_next++];
      (AllocEntry&)e = allocator.alloc(uint8_ts);
      e.flags = 0;
      return (void*)e.offset;
    }
    void free_persistent_raw(void* ptr)
    {
      AllocLogEntry& e = alloc_log[alloc_log_next++];
      e.flags = AllocLogEntry::RELEASE_FLAG;
      e.offset = (uint64_t)ptr;
      e.length = 0; // FIXME: learn length?
      allocator.free(e);
    }

    int start_transaction();

    int commit_transaction();
    int rollback_transaction();

    void queue_for_release(PObjBase* t, dtor destroy_fn)
    {
      objects2release->emplace_back(t, destroy_fn);
    }
    void queue_in_progress(PObjRecoverable* obj, TransactionId tid, void* ptr);
  };

  template <class T>
  class PObj : public PObjRecoverable
  {
  public:
    PObj(TransactionalRoot& t)
    {
      tid = t.get_effective_id();
      ptr = new (t) T();
    }
    PObj(TransactionId _tid, T* _ptr)
    {
      tid = _tid;
      ptr = _ptr;
    }
    inline const T* get() const {
      assert(tid != 0 && ptr);
      return reinterpret_cast<const T*>(ptr);
    }
    // just an alias for get()
    inline const T* operator->() const {
      return get();
    }
    inline T* access(TransactionalRoot& t) {
      assert(tid != 0 && ptr);
      auto _tid = t.get_effective_id();
      if (_tid == tid)
        return reinterpret_cast<T*>(ptr);

      // duplicate
      t.queue_in_progress(this, tid, reinterpret_cast<T*>(ptr));
      //t.queue_for_release(reinterpret_cast<T*>(ptr));
      t.queue_for_release(static_cast<T*>(ptr), [](const void* x) {
        static_cast<const T*>(x)->~T(); });

      tid = _tid;
      ptr = new (t) T(*reinterpret_cast<T*>(ptr));
      return reinterpret_cast<T*>(ptr);
    }
    inline void release(TransactionalRoot& t) {
      t.queue_in_progress(this, tid, reinterpret_cast<T*>(ptr));
      t.queue_for_release(this, [](const void* x) {
        static_cast<const PObj<T>*>(x)->~PObj<T>(); });
      t.queue_for_release(static_cast<T*>(ptr), [](const void* x) {
        static_cast<const T*>(x)->~T(); });
      tid = 0;
      ptr = nullptr;
    }
  };

  template <class T>
  class VPtr
  {
    uint64_t runId = 0;
    T* ptr = nullptr;
  public:
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
  template <class T>
  class PPtr
  {
    uint8_t* ptr = nullptr;
  public:
    PPtr(T* p)
    {
      reset(p);
    }
    PPtr(const PPtr& from)
    {
      ptr = from.ptr;
    }
    operator T*()
    {
      return reinterpret_cast<T*>(ptr + root->base);
    }
    operator const T*() const
    {
      return reinterpret_cast<const T*>(ptr + root->base);
    }

    T* operator ->()
    {
      return reinterpret_cast<T*>(ptr + root->base);
    }
    const T* operator ->() const
    {
      return reinterpret_cast<const T*>(ptr + root->base);
    }
    void reset(T* p)
    {
      assert((uint8_t)p >= root->base);
      ptr = (uint8_t*)p - root->base;
    }
  };

  template <class T, typename... Args>
  PObj<T>* alloc_persistent_obj(TransactionalRoot& tr, Args&&... args) {
    T* t = new (tr) T(args...);
    PObj<T>* tobj = new (tr) PObj<T>(tr.get_effective_id(), t);
    return tobj;
  }

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
      assert(working_transactional_root);
      return static_cast<pointer>(working_transactional_root->alloc_persistent_raw(count * sizeof(type)));
    }

    // Delete memory
    void deallocate(pointer ptr, size_type /* count */)
    {
      assert(working_transactional_root);
      working_transactional_root->free_persistent_raw(ptr);
    }
  };

  template <class T>
  class persistent_vector : public std::vector<T, persistent_allocator<T>>
  {};

  template <class T>
  class persistent_list : public std::list<T, persistent_allocator<T>>
  {};

}; // namespace PersistentObjects

#define CLASS_PERSISTENT_DECL(c) \
  class c; \
  typedef PersistentObjects::PObj<c>* c##Ptr;

#define CLASS_PERSISTENT(c) \
  CLASS_PERSISTENT_DECL(c)  \
  class c : public PersistentObjects::PObjBase


#endif

