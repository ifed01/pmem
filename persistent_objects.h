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

#include <iostream>
//#include <memory>

#ifdef __GNUC__
#include <boost/interprocess/offset_ptr.hpp>
using namespace boost::interprocess;
#endif

namespace PersistentObjects
{
  //using namespace std;
  struct AllocEntry
  {
    uint64_t offset = 0;
    uint32_t length = 0;
    AllocEntry() {}
    AllocEntry(uint64_t o, uint32_t l) : offset(o), length(l) {}
  };
  class TransactionalAllocator
  {
    size_t alloc_cnt = 0;
  public:
    AllocEntry alloc(size_t uint8_ts);
    void free(const AllocEntry& e);
    void note_alloc(const AllocEntry& e)
    {
    }
    void apply_release(const AllocEntry& e)
    {
      // FIXME
    }
    size_t get_alloc_count() const
    {
      return alloc_cnt;
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
    void* p = nullptr; // this is PObjBase if destroy_fn != null and void* overwise
    dtor destroy_fn = nullptr;
    PObjBaseDestructor(void* _p, dtor _destroy_fn) :
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
    uint64_t base = 0;
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
    std::atomic<int> readers_count;
    bool in_transaction = false;

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
#define LOCK_READ
#define UNLOCK_READ

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
      // permit within transaction scope only
      assert(in_transaction);
      AllocLogEntry& e = alloc_log[alloc_log_next++];
      (AllocEntry&)e = allocator.alloc(uint8_ts);
      e.flags = 0;
      return (void*)e.offset;
    }
    void free_persistent_raw(void* ptr)
    {
      // permit within transaction scope only
      assert(in_transaction);
      AllocLogEntry& e = alloc_log[alloc_log_next++];
      e.flags = AllocLogEntry::RELEASE_FLAG;
      e.offset = (uint64_t)ptr;
      e.length = 0; // FIXME: learn length?
      allocator.free(e);
    }

    int start_read_access();
    int stop_read_access();

    int start_transaction();

    int commit_transaction();
    int rollback_transaction();

    void queue_for_release(PObjBase* t, dtor destroy_fn)
    {
      objects2release->emplace_back(t, destroy_fn);
    }
    void queue_for_release(void* t)
    {
      objects2release->emplace_back(t, nullptr);
    }
    void queue_in_progress(PObjRecoverable* obj, TransactionId tid, void* ptr);
    size_t get_object_count() const
    {
      return allocator.get_alloc_count();
    }
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
    // just an alias for inspect()
    inline const T* operator->() const {
      return inspect();
    }
    operator const T&() const
    {
      return *inspect();
    }

    inline const T* inspect() const {
      assert(tid != 0 && ptr);
      return reinterpret_cast<const T*>(ptr);
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
      T& tmp = *reinterpret_cast<T*>(ptr);
      ptr = new (t) T(*reinterpret_cast<T*>(ptr));
      return reinterpret_cast<T*>(ptr);
    }
    inline void die(TransactionalRoot& t) {
      assert(ptr);
      t.queue_in_progress(this, tid, reinterpret_cast<T*>(ptr));
      t.queue_for_release(this, [](const void* x) {
        static_cast<const PObj<T>*>(x)->~PObj<T>(); });
      t.queue_for_release(static_cast<T*>(ptr), [](const void* x) {
        static_cast<const T*>(x)->~T(); });

      // dtor simulation.
      // we can probably inspect rid off it by enforcing ref_counted ptr usage
      // inside persisent objects
      reinterpret_cast<T*>(ptr)->die(t);

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
  class PPtrRootOffset
  {
    T* ptr = nullptr;

  public:

    typedef T              element_type;
    typedef T              value_type;

    PPtrRootOffset()
    {
    }
    PPtrRootOffset(nullptr_t)
    {
    }
    PPtrRootOffset(T* p)
    {
      if (p == nullptr) {
        ptr = p;
      }
      else {
        assert((uint64_t)p >= root->base);
        ptr = (T*)((uint8_t*)p - root->base);
      }
    }

    PPtrRootOffset(const PPtrRootOffset& from)
    {
      ptr = from.ptr;
    }
    PPtrRootOffset<T> operator++()
    {
      ptr++;
      return this;
    }
    operator T*() const
    {
      return reinterpret_cast<T*>((uint8_t*)ptr + root->base);
    }
    T* operator ->() const
    {

      return reinterpret_cast<T*>((uint8_t*)ptr + root->base);
    }
    size_t operator -(const PPtrRootOffset<T>& p) const
    {
      return (ptr - p.ptr);

    }
    PPtrRootOffset<T>& operator +(size_t delta) const
    {
      PPtrRootOffset<T> p(ptr + delta);
      return p;
    }

    bool operator !=(const PPtrRootOffset<T>& p) const
    {
      return ptr != p.ptr;
    }
    bool operator !=(nullptr_t) const
    {
      return ptr != nullptr;
    }
    template <typename U>
    static PPtrRootOffset<T> pointer_to(U& r)
    {
      return PPtrRootOffset<T>(&r);
    }

    template <class U, typename... Args>
    static PPtrRootOffset<PObj<U>> alloc_persistent_obj(TransactionalRoot& tr, Args&&... args) {
      U* t = new (tr) U(args...);
      return new (tr) PObj<U>(tr.get_effective_id(), t);
    }
    static PPtrRootOffset<T> alloc_persistent_count(TransactionalRoot& tr, size_t count) {
      return PPtrRootOffset<T>((T*)tr.alloc_persistent_raw(count * sizeof(T)));
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
    static PPtrThisOffset<PObj<U>> alloc_persistent_obj(TransactionalRoot& tr, Args&&... args) {
      U* t = new (tr) U(args...);
      return new (tr) PObj<U>(tr.get_effective_id(), t);
    }
    static PPtrThisOffset<T> alloc_persistent_count(TransactionalRoot& tr, size_t count) {
      return PPtrThisOffset<T>((T*)tr.alloc_persistent_raw(count * sizeof(T)));
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
    static PBoostOffsetPtr<PObj<U>> alloc_persistent_obj(TransactionalRoot& tr, Args&&... args) {
      U* t = new (tr) U(args...);
      return new (tr) PObj<U>(tr.get_effective_id(), t);
    }
    static PBoostOffsetPtr<T> alloc_persistent_count(TransactionalRoot& tr, size_t count) {
      return PBoostOffsetPtr<T>((T*)tr.alloc_persistent_raw(count * sizeof(T)));
    }
  };
  template <typename T>
  using PPtr = PBoostOffsetPtr<T>;
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
      assert(working_transactional_root);
      //return static_cast<pointer>(working_transactional_root->alloc_persistent_raw(count * sizeof(type)));
      //pointer p;
      //p.reset(working_transactional_root->alloc_persistent_raw(count * sizeof(type)));
      //return p;

      //return pointer((T*)working_transactional_root->alloc_persistent_raw(count * sizeof(type)));
      return pointer::alloc_persistent_count(*working_transactional_root, count);
    }

    // Delete memory
    void deallocate(pointer ptr, size_type /* count */)
    {
      //std::cout << "deallocate"<< (uint64_t)(T*)ptr << std::endl;
      assert(working_transactional_root);
      working_transactional_root->queue_for_release((void*)ptr);
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
      assert(working_transactional_root);
      //return static_cast<pointer>(working_transactional_root->alloc_persistent_raw(count * sizeof(type)));
      return pointer(working_transactional_root->alloc_persistent_raw(count * sizeof(type)));
    }

    // Delete memory
    void deallocate(pointer ptr, size_type /* count */)
    {
      assert(working_transactional_root);
      working_transactional_root->queue_for_release((void*)ptr);
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
  using persistent_map2 = std::map<K, V, std::less<K>, persistent_allocator2<std::pair<K, V>>>;
}; // namespace PersistentObjects

#define PERSISTENT_CLASS_DECL(c) \
  class c; \
  typedef PersistentObjects::PPtr<PersistentObjects::PObj<c>> c##Ptr;

#define PERSISTENT_CLASS(c) \
  PERSISTENT_CLASS_DECL(c)  \
  class c : public PersistentObjects::PObjBase

#define PERSISTENT_DEAD void die(TransactionalRoot& tr)

#define PERSISTENT_DIE(a) { \
  if (a != nullptr) {\
    a->die(tr);\
  }\
}
#endif
