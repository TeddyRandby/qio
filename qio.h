#ifndef QIO_H
#define QIO_H

#define QIO_API static inline

#include <stdatomic.h>
#include <stdint.h>

/*
 *  ██████╗ ██╗ ██████╗
 *  ██╔═══██╗██║██╔═══██╗
 *  ██║   ██║██║██║   ██║
 *  ██║▄▄ ██║██║██║   ██║
 *  ╚██████╔╝██║╚██████╔╝
 *   ╚══▀▀═╝ ╚═╝ ╚═════╝
 *
 * QIO is an experimental, cross-platform, and header-only library for
 * performing asynchronous IO. It was designed to meet the needs of the gab
 * programming language (https://gab-language.github.io/site/).
 *
 * Why write a new async-io library for c? libuv exists, and theres other
 * options beyond that. So why create something new?
 *
 * - Simple and low-level api
 * - No callback hell
 * - Threadsafe
 * - Header only
 *
 * QIO's implementation is comprised of two parts: io-operation *producers* and
 * a single io-operation *consumer*.
 *
 * A consumer is an OS thread *dedicated* to performing IO operations. The
 * relevant consumer functions are as follows:
 *
 *  int32_t qio_init(uint64_t size);
 *
 *    qio_init initializes qio's global state, and any operating-system specific
 *    data structures that are necessary for qio to function. On linux, this
 *    function creates the io_uring, and on macos it creates the kqueue.
 *
 *  int32_t qio_loop();
 *
 *    qio_loop is the event-loop that does all the io-work. This is a *blocking*
 *    function. It will only return if qio encounters some internal error and
 * can *no longer* correctly do any io.
 *
 *  void qio_destroy();
 *
 *    qio_destroy (theoretically) cleans up the data structures initialized by
 *    qio_init. Honestly there isn't much point to this function, as the only
 *    reason you'd ever clean this up in your application is if you're done
 * doing IO (and at this point youre about to exit the io thread anyway).
 *
 *    For this reason, qio_destroy is actually a no-op for now.
 *
 * A producer is any other thread which calls these producing functions. These
 * functions are designed to mirror the normal posix functions you're already
 * used to - just add a 'q' in front to make it async!
 *
 *  qd_t qsend(qfd_t fd, uint64_t n, const uint8_t buf[n]);
 *
 *    qsend is the async version (producer) of the corresponding send function.
 * The fundamental difference between these queued functions and their
 * synchronous counterparts is that all the queued functions return a `qd_t`.
 *
 * This `qd_t` is the handle that corresponds to the `queued` operation. There
 * are a few functions which operate on the qd (pronounced 'kid').
 *
 *  int8_t  qd_status(qd_t qd);
 *
 *    qd_status is *not* blocking, and checks on the status of a qd (a queued io
 *    operation). It returns non-zero if the operation is complete, and zero if
 * the operation is in-progress.
 *
 *  int64_t qd_result(qd_t qd);
 *
 *    qd_result is *blocking*. It blocks the caller until the qd is complete,
 *    and then returns the result of the operation (as if you called the
 *    corresponding synchronous function).
 *
 *  int64_t qd_destroy(qd_t qd);
 *
 *    This function frees qio's data associated with the given qd. Practically,
 * there is no 'malloc' or 'free' actually happening.
 *
 *    All of qio's qds are allocated in a single contiguous vector. This vector
 * grows as more qds are needed, and never shrinks. qd_destroy marks slots in
 * this vector as 'free', which qio will re-use for later operations. This is
 * implemented with an intrusive 'free list'.
 *
 * All of the producer and qd helper functions are *thread-safe*.
 * QIO is designed for applications to spawn a single io-consumer thread,
 * and then call the producer functions from any number of other threads.
 *
 * Below is a typical IO loop function for qio. It uses an atomic bool* to
 * notify parent thread when qio initialization is complete, and then the
 * application may begin calling producers.
 *
 *  int io_loop(void *initialized) {
 *    if (qio_init(QSIZE) < 0)
 *      return 1;
 *
 *    *(_Atomic bool *)initialized = true;
 *
 *    if (qio_loop() < 0)
 *      return qio_destroy(), 1;
 *
 *    return qio_destroy(), 0;
 *  }
 *
 * Simply spawn a thread with this function using your os threads api (or the
 * standard <threads.h>)
 *
 *  _Atomic bool initialized = false;
 *  thrd_t io_t;
 *  if (thrd_create(&io_t, io_loop, &initialized) != thrd_success)
 *    // handle the error
 *
 *  while (!initialized)
 *    ; // wait
 *
 *  // Do all your io forever now, from whatever thread you want!
 *
 * NOTE:
 *  There is an additional type, a `qfd_t`, that the producing functions often
 * use. This type is an abstraction over the OS's native file/socket type. IE,
 * it is an int on unix-like systems, and a HANDLE on windows (windows
 * implementation incomplete).
 *
 * DEPENDENCIES:
 *  This repo includes two other header files in include/.
 *
 *  The first is vector.h, a generic macro implementation of a vector data
 * structure for c. This is used in QIO's internal data structures.
 *
 *  The second is threads.h. Since the <threads.h> header is optional in the
 * c-standard, not all platforms provide implementations. This header provides a
 * cross-platform implementation if you are building for one of these lazy
 * platforms <ahem, macos>. QIO itself doesn't need <threads.h>, but it is
 * useful for any cross-platform application which *would use* QIO.
 *
 * TODO:
 *  - The qd freelist is wrapped by a mutex to make it thread-safe.
 *    This could probably be improved by using an atomic qd as the *head of the
 * list*, meaning we could atomically pop items out without having to lock.
 * Maybe there is an entirely lock-free implementation of the freelist we could
 * use as well.
 *  - The windows implementation with IO completion ports.
 *  - Double check that qio_addrfrom is implemented correctly. I really don't
 * know.
 *  - Further testing and benchmarking
 *  - Implement file-permission flags for qopen/qopenat
 *  - Implement more producer functions. (Maybe the io_vec stuff would be
 * useful?)
 *  - Implement cross-platform error string retrieval. (Wrapping strerror, etc)
 */

/*
 * A qid (pronounced 'kid') is a handle representing one IO operation.
 * It is used to:
 *  - Check on the status of its corresponding operation.
 *  - Get the result of its operation
 */
typedef int32_t qd_t;

/*
 * An abstraction over an os-specific file-descriptor.
 */
typedef int64_t qfd_t;

/*
 * Initialize QIO. This should only be called *once* per thread.
 *
 * This sets up platform-specific IO datastructures. (Like the queues in
 * io_uring).
 */
QIO_API int32_t qio_init(uint64_t size);
QIO_API int32_t qio_loop();
QIO_API void qio_destroy();

struct qio_addr;
QIO_API int qio_addrfrom(const char *restrict src, uint16_t port,
                         struct qio_addr *dst);

/*
 * The following are the 'queued' versions of corresponding POSIX functions.
 */
QIO_API qd_t qopen(const char *path);
QIO_API qd_t qopenat(qfd_t fd, const char *path);

QIO_API qd_t qread(qfd_t fd, uint64_t n, uint8_t buf[n]);
QIO_API qd_t qwrite(qfd_t fd, uint64_t n, const uint8_t buf[n]);

struct qio_stat;
QIO_API qd_t qstat(qfd_t fd, struct qio_stat *stat);

enum qsock_type { QSOCK_TCP, QSOCK_UDP };
QIO_API qd_t qsocket(enum qsock_type type);

QIO_API qd_t qbind(qfd_t fd, const struct qio_addr *addr);
QIO_API qd_t qlisten(qfd_t fd, uint32_t backlog);
QIO_API qd_t qaccept(qfd_t fd, struct qio_addr *addr_out);

QIO_API qd_t qconnect(qfd_t fd, const struct qio_addr *addr);

QIO_API qd_t qsend(qfd_t fd, uint64_t n, const uint8_t buf[n]);
QIO_API qd_t qrecv(qfd_t fd, uint64_t n, uint8_t buf[n]);

struct qio_op_t {
  /* Has the io op completed? */
  int8_t done;

  /*
   * BOO wasted space
   */

  union {
    /* OS return values */
    int64_t result;
    /* Next free item in the free list. */
    qd_t next_free;
  };
};

#define T struct qio_op_t
#define NAME qd
#define V_CONCURRENT
#include "vector.h"

mtx_t _qio_freelist_mtx;

/*
 * The following variable (and other platform-specific globals like it)
 * are marked static, and not _Thread_local. This is intentional -
 * it is almost *always* the case that IO operations qre queued from
 * a different thread than the one running the qio_loop. Because of this,
 * these data structures need to be thread-safe and static.
 */
static v_qd _qio_qds = {0};
static uint32_t _qio_qd_free = (uint32_t)-1;

/*
 * This function is *not* blocking. It will immediately return:
 *  - nonzero if the corresponding operation is complete.
 *  - zero if the operation is still in progress.
 */
QIO_API int8_t qd_status(qd_t qd) {
  assert(qd < _qio_qds.len);
  return v_qd_val_at(&_qio_qds, qd).done;
}

/*
 * This operation is *blocking*. It block the caller until the qid's
 * corresponding operation is complete.
 */
QIO_API int64_t qd_result(qd_t qd) {
  assert(qd < _qio_qds.len);

  // Simply block until we have received a response from the os.
  while (!qd_status(qd))
    ;

  return v_qd_val_at(&_qio_qds, qd).result;
}

static const uint32_t negative_one = -1;
static const struct qio_op_t destroyed = {.next_free = negative_one};

/* the O(n) operation, appending to list */
static inline void _freelist_push(qd_t qd) {
  assert(_qio_qd_free != negative_one);

  struct qio_op_t op = v_qd_val_at(&_qio_qds, _qio_qd_free);
  qd_t p_qd = _qio_qd_free;

  while (op.next_free != negative_one) {
    p_qd = op.next_free;
    op = v_qd_val_at(&_qio_qds, op.next_free);
    assert(!op.done);
  }

  assert(op.next_free == negative_one);
  assert(p_qd != negative_one);
  assert(p_qd < _qio_qds.len);

  op.next_free = qd;
  v_qd_set(&_qio_qds, p_qd, op);
}

/* The O(1) operation, popping from list */
static inline qd_t _freelist_pop() {
  assert(_qio_qd_free != negative_one);

  qd_t qd = _qio_qd_free;

  struct qio_op_t op = v_qd_val_at(&_qio_qds, _qio_qd_free);
  _qio_qd_free = op.next_free;

  v_qd_set(&_qio_qds, qd, (struct qio_op_t){});

  return qd;
}

QIO_API int64_t qd_destroy(qd_t qd) {
  assert(qd < _qio_qds.len);

  /* Block until the operation is done. */
  int64_t result = qd_result(qd);

  assert(v_qd_val_at(&_qio_qds, qd).done);

  mtx_lock(&_qio_freelist_mtx);

  v_qd_set(&_qio_qds, qd, destroyed);

  if (_qio_qd_free == negative_one)
    return _qio_qd_free = qd, (void)mtx_unlock(&_qio_freelist_mtx), result;

  return _freelist_push(qd), mtx_unlock(&_qio_freelist_mtx), result;
}

QIO_API qd_t qd_next() {
  mtx_lock(&_qio_freelist_mtx);

  if (_qio_qd_free != negative_one) {
    qd_t qd = _freelist_pop();

    if (qd != negative_one) {

      v_qd_set(&_qio_qds, qd, (struct qio_op_t){});

      mtx_unlock(&_qio_freelist_mtx);

      return qd;
    }
  }

  qd_t qd = v_qd_push(&_qio_qds, (struct qio_op_t){});
  assert(_qio_qds.len > 0);

  return mtx_unlock(&_qio_freelist_mtx), qd;
}

#ifndef QIO_LOOP_INTERVAL_NS
#define QIO_LOOP_INTERVAL_NS 500000
#endif

#ifndef QIO_INTERNAL_QUEUE_INITIAL_LEN
#define QIO_INTERNAL_QUEUE_INITIAL_LEN 1024
#endif

#ifdef QIO_LINUX

/*
 * Linux implementation using io_uring.
 *
 * A thread-safe vector of submission-queue-entries (sqe)
 * holds the requests made by producers.
 *
 * The consumer wakes up, drains *all* entries, and queues
 * as many as it can to the ring. It then checks for any
 * completion-queue-entries, and resolves these qds.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/io_uring.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

#define io_uring_smp_store_release(p, v)                                       \
  atomic_store_explicit((_Atomic typeof(*(p)) *)(p), (v), memory_order_release)

#define io_uring_smp_load_acquire(p)                                           \
  atomic_load_explicit((_Atomic typeof(*(p)) *)(p), memory_order_acquire)

struct qio_addr {
  struct sockaddr_in6 addr;
  socklen_t len;
};

struct qio_stat {
  struct statx statxbuf;
};

QIO_API uint64_t qio_statsize(struct qio_stat *stat) {
  assert(stat->statxbuf.stx_mask & STATX_SIZE);
  return stat->statxbuf.stx_size;
}

QIO_API int qio_addrfrom(const char *restrict src, uint16_t port,
                         struct qio_addr *dst) {
  struct addrinfo *addrinfo;

  struct addrinfo hints = {.ai_family = AF_INET6};

  // TODO: Maybe this needs to include AI_PASSIVE inorder to work for bind?
  // struct addrinfo hints = {.ai_family = AF_INET6, .ai_flags = AI_PASSIVE};

  int s = getaddrinfo(src, nullptr, &hints, &addrinfo);
  if (s != 0)
    return s;

  if (addrinfo == nullptr)
    return -1;

  struct sockaddr_in6 *saddr = (struct sockaddr_in6 *)addrinfo->ai_addr;
  assert(addrinfo->ai_addrlen <= sizeof(dst->addr));

  memcpy(&dst->addr, addrinfo->ai_addr, addrinfo->ai_addrlen);

  dst->len = addrinfo->ai_addrlen;
  dst->addr.sin6_port = htons(port);

  return freeaddrinfo(addrinfo), 0;
}

static qfd_t _qio_ring;

static uint32_t *_qio_sring_tail, *_qio_sring_head, *_qio_sring_mask,
    *_qio_sring_array, *_qio_cring_head, *_qio_cring_tail, *_qio_cring_mask;

static struct io_uring_sqe *_qio_sqes;
static struct io_uring_cqe *_qio_cqes;
static uint32_t _qio_ring_entries;

#define T struct io_uring_sqe
#define NAME sqe
#define V_CONCURRENT
#include "vector.h"

static v_sqe _qio_queued_sqes;

/* FIXME: Figure out a better syscall intrinsic system */
int io_uring_setup(unsigned entries, struct io_uring_params *p) {
  return (int)syscall(__NR_io_uring_setup, entries, p);
}

int io_uring_enter(unsigned int fd, unsigned int to_submit,
                   unsigned int min_complete, unsigned int flags,
                   sigset_t *sig) {
  return (int)syscall(__NR_io_uring_enter, fd, to_submit, min_complete, flags,
                      sig);
}

QIO_API int32_t qio_loop() {
  struct timespec interval = {.tv_nsec = QIO_LOOP_INTERVAL_NS};

  while (true) {
    /* Read barrier */
    unsigned head = io_uring_smp_load_acquire(_qio_cring_head);

    /*
     * If we don't have any ops to queue and the receive buffer is empty:
     * Sleep and try again later.
     */
    if (!_qio_queued_sqes.len && head == *_qio_cring_tail) {
      thrd_sleep(&interval, nullptr);
      continue;
    }

    /*
     * If we have opts to queue
     */
    if (_qio_queued_sqes.len) {
      /*
       * Atomically drain all our queued sqes into a local buffer.
       */
      v_sqe buffered;
      v_sqe_drain(&_qio_queued_sqes, &buffered);

      // Rudimentary assert here.
      // It would be better to just put everything we can in ring,
      // and re-queue the rest.
      assert(buffered.len < _qio_ring_entries);

      for (int i = 0; i < buffered.len; i++) {
        struct io_uring_sqe *src_sqe = &buffered.data[i];

        unsigned index, tail;
        tail = *_qio_sring_tail;
        index = tail & *_qio_sring_mask;

        assert(src_sqe->user_data < _qio_qds.len);
        struct io_uring_sqe *dst_sqe = &_qio_sqes[index];
        memcpy(dst_sqe, src_sqe, sizeof(struct io_uring_sqe));

        _qio_sring_array[index] = index;
        tail++;

        /* Update the tail */
        io_uring_smp_store_release(_qio_sring_tail, tail);
      }

      /* System call to trigger kernel */
      if (buffered.len)
        io_uring_enter(_qio_ring, buffered.len, 0, 0, nullptr);

      /* Free our buffered list */
      v_sqe_destroy(&buffered);
    }

    /*
     * If we have ops completed
     */
    if (head != *_qio_cring_tail) {
      /* Get the entry */
      unsigned index = head & (*_qio_cring_mask);
      struct io_uring_cqe *cqe = &_qio_cqes[index];
      head++;

      qd_t qid = cqe->user_data;

      assert(qid < _qio_qds.len);
      assert(v_qd_val_at(&_qio_qds, qid).done == false);
      assert(v_qd_val_at(&_qio_qds, qid).result == 0);
      /*
       * Perform this via a set for two resons:
       *  - The struct is small enough that passying and copying by value is
       * fine
       *  - This performes the update in *one atomic operation*. Holding
       * pointers into the vector is *unsafe* in a concurrent vector, as the
       * array could be reallocated out from underneath you.
       */
      v_qd_set(&_qio_qds, qid,
               (struct qio_op_t){
                   .result = cqe->res == -EINTR ? 0 : cqe->res,
                   .done = true,
               });

      /* Write barrier so that update to the head are made visible */
      io_uring_smp_store_release(_qio_cring_head, head);
    }
  }
}

/* NOOP */
QIO_API void qio_destroy() {}

QIO_API int32_t qio_init(uint64_t size) {
  /* Initialize qds. All OS's need to do this. */
  v_qd_create(&_qio_qds, QIO_INTERNAL_QUEUE_INITIAL_LEN);
  mtx_init(&_qio_freelist_mtx, mtx_plain);

  _qio_ring_entries = size;

  struct io_uring_params p = {0};
  /* The submission and completion queue */
  void *sq, *cq;

  _qio_ring = io_uring_setup(_qio_ring_entries, &p);
  if (_qio_ring < 0)
    return -1;

  int sring_sz = p.sq_off.array + p.sq_entries * sizeof(unsigned);
  int cring_sz = p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe);

  if (p.features & IORING_FEAT_SINGLE_MMAP) {
    if (cring_sz > sring_sz)
      sring_sz = cring_sz;
    cring_sz = sring_sz;
  }
  sq = mmap(0, sring_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
            _qio_ring, IORING_OFF_SQ_RING);
  if (sq == MAP_FAILED)
    return -1;

  assert(sq != nullptr);

  if (p.features & IORING_FEAT_SINGLE_MMAP) {
    cq = sq;
  } else {
    /* Map in the completion queue ring buffer in older kernels separately */
    cq = mmap(0, cring_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
              _qio_ring, IORING_OFF_CQ_RING);
    if (cq == MAP_FAILED)
      return -1;
  }

  _qio_sring_tail = (uint32_t *)((uint8_t *)sq + p.sq_off.tail);
  _qio_sring_head = (uint32_t *)((uint8_t *)sq + p.sq_off.head);
  _qio_sring_mask = (uint32_t *)((uint8_t *)sq + p.sq_off.ring_mask);
  _qio_sring_array = (uint32_t *)((uint8_t *)sq + p.sq_off.array);

  _qio_sqes = (struct io_uring_sqe *)mmap(
      0, p.sq_entries * sizeof(struct io_uring_sqe), PROT_READ | PROT_WRITE,
      MAP_SHARED | MAP_POPULATE, _qio_ring, IORING_OFF_SQES);

  if (_qio_sqes == MAP_FAILED)
    return -1;

  assert(cq != nullptr);

  _qio_cring_head = (uint32_t *)((uint8_t *)cq + p.cq_off.head);
  _qio_cring_tail = (uint32_t *)((uint8_t *)cq + p.cq_off.tail);
  _qio_cring_mask = (uint32_t *)((uint8_t *)cq + p.cq_off.ring_mask);
  _qio_cqes = (struct io_uring_cqe *)((uint8_t *)cq + p.cq_off.cqes);

  return 0;
}

qd_t _qio_append_sqe(struct io_uring_sqe *src_sqe) {
  qd_t qid = qd_next();
  assert(_qio_qds.len > 0);

  src_sqe->user_data = qid;
  v_sqe_push(&_qio_queued_sqes, *src_sqe);

  return qid;
}

QIO_API qd_t qopen(const char *path) { return qopenat(AT_FDCWD, path); }

QIO_API qd_t qopenat(qfd_t fd, const char *path) {
  return _qio_append_sqe(&(struct io_uring_sqe){
      .opcode = IORING_OP_OPENAT,
      .fd = fd,
      .addr = (uintptr_t)path,
      .len = 0666, // Created file permissions
      .open_flags = O_RDWR | O_CREAT | O_APPEND,
  });
}

/*
 * Some of these SQE's have arguments in places that make sense (read, write).
 * Some of them are all over the place (socket, accept).
 * Check liburing on github for useful examples of how to create SQE's for every
 * IO_URING op.
 */

QIO_API qd_t qread(qfd_t fd, uint64_t n, uint8_t buf[n]) {
  assert(n < UINT32_MAX);
  return _qio_append_sqe(&(struct io_uring_sqe){
      .opcode = IORING_OP_READ,
      .fd = fd,
      .addr = (uintptr_t)buf,
      .len = n,
      .off = -1,
  });
}

QIO_API qd_t qstat(qfd_t fd, struct qio_stat *stat) {
  return _qio_append_sqe(&(struct io_uring_sqe){
      .opcode = IORING_OP_STATX,
      .fd = fd,
      .addr = (uintptr_t)"",
      .statx_flags = AT_EMPTY_PATH,
      .len = STATX_BASIC_STATS,
      .off = (uintptr_t)stat,
  });
}

QIO_API qd_t qwrite(qfd_t fd, uint64_t n, const uint8_t buf[n]) {
  assert(n < UINT32_MAX);
  return _qio_append_sqe(&(struct io_uring_sqe){
      .opcode = IORING_OP_WRITE,
      .fd = fd,
      .addr = (uintptr_t)buf,
      .len = n,
      .off = -1,
  });
}

QIO_API qd_t qsend(qfd_t fd, uint64_t n, const uint8_t buf[n]) {
  assert(n < UINT32_MAX);
  return _qio_append_sqe(&(struct io_uring_sqe){
      .opcode = IORING_OP_SEND,
      .fd = fd,
      .addr = (uintptr_t)buf,
      .len = n,
  });
}

QIO_API qd_t qrecv(qfd_t fd, uint64_t n, uint8_t buf[n]) {
  return _qio_append_sqe(&(struct io_uring_sqe){
      .opcode = IORING_OP_RECV,
      .fd = fd,
      .addr = (uintptr_t)buf,
      .len = n,
  });
}

QIO_API qd_t qsocket(enum qsock_type type) {
  int os_type;

  switch (type) {
  case QSOCK_TCP:
    os_type = SOCK_STREAM;
    break;
  case QSOCK_UDP:
    os_type = SOCK_DGRAM;
    break;
  default:
    return -1;
  }

  return _qio_append_sqe(&(struct io_uring_sqe){
      .opcode = IORING_OP_SOCKET,
      .fd = AF_INET6,
      .len = 0,
      .off = os_type,
  });
}

QIO_API qd_t qclose(qfd_t fd) {
  return _qio_append_sqe(&(struct io_uring_sqe){
      .opcode = IORING_OP_CLOSE,
      .fd = fd,
  });
}

QIO_API qd_t qshutdown(qfd_t fd) {
  return _qio_append_sqe(&(struct io_uring_sqe){
      .opcode = IORING_OP_SHUTDOWN,
      .fd = fd,
      .len = SHUT_RDWR,
  });
}

QIO_API qd_t qlisten(qfd_t fd, uint32_t backlog) {
  return _qio_append_sqe(&(struct io_uring_sqe){
      .opcode = IORING_OP_LISTEN,
      .fd = fd,
      .len = backlog,
  });
}

QIO_API qd_t qbind(qfd_t fd, const struct qio_addr *addr) {
  return _qio_append_sqe(&(struct io_uring_sqe){
      .opcode = IORING_OP_BIND,
      .fd = fd,
      .addr = (uintptr_t)&addr->addr,
      .off = addr->len,
  });
}

QIO_API qd_t qaccept(qfd_t fd, struct qio_addr *addr_out) {
  return _qio_append_sqe(&(struct io_uring_sqe){
      .opcode = IORING_OP_ACCEPT,
      .fd = fd,
      .addr = (uintptr_t)&addr_out->addr,
      .off = (uintptr_t)&addr_out->len,
  });
}

QIO_API qd_t qconnect(qfd_t fd, const struct qio_addr *addr) {
  return _qio_append_sqe(&(struct io_uring_sqe){
      .opcode = IORING_OP_CONNECT,
      .fd = fd,
      .addr = (uintptr_t)&addr->addr,
      .off = addr->len,
  });
}

#elifdef QIO_MACOS

/*
 * The MACOS implementation uses kqueue.
 *
 * There is a similar thread-safe vector of kqueue- operations that our
 * producers append to.
 *
 * The consumer loopwakes up, drains the vector, and processes the requests.
 *
 * Some requests don't need to wait for IO (such as qsocket, qopen, etc).
 *
 * These requests are completed when the are processed at this point.
 *
 * Requests that *do* need to wait for IO (qsend, qrecv, qwrite, qread) are
 * collected and sent to the kqueue, to track when the fd is readable/writable.
 *
 * When we get an event from kequeue, we complete the operation we were waiting
 * on.
 *
 */

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

struct qio_addr {
  struct sockaddr_in6 addr;
  socklen_t len;
};

// Implementation is the same as linux
QIO_API int qio_addrfrom(const char *restrict src, uint16_t port,
                         struct qio_addr *dst) {
  struct addrinfo *addrinfo;

  struct addrinfo hints = {.ai_family = AF_INET6};
  // TODO: Maybe this needs to include AI_PASSIVE inorder to work for bind?
  // struct addrinfo hints = {.ai_family = AF_INET6, .ai_flags = AI_PASSIVE};

  int s = getaddrinfo(src, nullptr, &hints, &addrinfo);
  if (s != 0)
    return s;

  if (addrinfo == nullptr)
    return -1;

  struct sockaddr_in *saddr = (struct sockaddr_in *)addrinfo->ai_addr;
  assert(addrinfo->ai_addrlen <= sizeof(dst->addr));
  memcpy(&dst->addr, addrinfo->ai_addr, addrinfo->ai_addrlen);
  dst->len = addrinfo->ai_addrlen;
  dst->addr.sin6_port = htons(port);
  return freeaddrinfo(addrinfo), 0;
}

struct qio_stat {
  struct stat statbuf;
};

qfd_t _qio_queue;
QIO_API uint64_t qio_statsize(struct qio_stat *stat) {
  return stat->statbuf.st_size;
}

struct qio_kevent {
  struct kevent ke;

  enum {
    QIO_KQ_OPENAT,
    QIO_KQ_READ,
    QIO_KQ_WRITE,
    QIO_KQ_STAT,
    QIO_KQ_SOCKET,
    QIO_KQ_ACCEPT,
    QIO_KQ_LISTEN,
    QIO_KQ_BIND,
    QIO_KQ_CONNECT,
    QIO_KQ_CLOSE,
    QIO_KQ_SHUTDOWN,
    QIO_KQ_SEND,
    QIO_KQ_RECV,
  } op;

  union {
    struct {
      struct stat *buf;
    } stat;

    struct {
      const char *path;
    } openat;

    struct {
      uint64_t n;
      uint8_t *buf;
    } read;

    struct {
      uint64_t n;
      const uint8_t *buf;
    } write;

    struct {
      int type;
    } socket;

    struct {
      struct qio_addr *addr_out;
    } accept;

    struct {
      const struct qio_addr *addr;
    } connect;

    struct {
      const struct qio_addr *addr;
    } bind;

    struct {
      uint32_t backlog;
    } listen;

    struct {
    } close;

    struct {
      int32_t how;
    } shutdown;

    struct {
      uint64_t n;
      const uint8_t *buf;
    } send;

    struct {
      uint64_t n;
      uint8_t *buf;
    } recv;
  };
};

#define T struct qio_kevent
#define NAME kevent
#define V_CONCURRENT
#include "vector.h"

v_kevent _qio_pending_ops;

QIO_API void qio_destroy() {}

int setfd_nonblock(int fd) {
  int flags = fcntl(fd, F_GETFL);
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

QIO_API int32_t qio_init(uint64_t size) {
  v_qd_create(&_qio_qds, QIO_INTERNAL_QUEUE_INITIAL_LEN);
  mtx_init(&_qio_freelist_mtx, mtx_plain);

  v_kevent_create(&_qio_pending_ops, QIO_INTERNAL_QUEUE_INITIAL_LEN);

  int res;
  if ((res = setfd_nonblock(STDIN_FILENO)) < 0)
    return res;

  if ((res = setfd_nonblock(STDOUT_FILENO)) < 0)
    return res;

  if ((res = setfd_nonblock(STDERR_FILENO)) < 0)
    return res;

  _qio_queue = kqueue();

  if (_qio_queue < 0)
    return _qio_queue;

  return 0;
}

/**
 * Finish the qd stored in ke->ke.udata with res and flags;
 */
void resolve_qio_kevent(struct qio_kevent *ke, int64_t res) {
  qd_t qd = (uintptr_t)ke->ke.udata;
  struct qio_op_t op = v_qd_val_at(&_qio_qds, qd);

  op.result = res < 0 ? -errno : res;
  op.done = true;

  v_qd_set(&_qio_qds, qd, op);
}

void resolve_polled(struct kevent *events, int nevents) {
  v_kevent pending;

  // Drain the pending ops into our local vector.
  // This atomically empties the pending_ops vector,
  // Allowing other threads to queue more pending operations
  // while this vector is processed.
  v_kevent_drain(&_qio_pending_ops, &pending);

  for (int j = 0; j < pending.len; j++) {
    struct qio_kevent *qioke = &pending.data[j];
    qd_t qd = (uintptr_t)qioke->ke.udata;

    ssize_t result;

    /* Search for a matching event amongst our given events */
    // FIXME: O(n) search here no good.
    for (int i = 0; i < nevents; i++) {
      struct kevent *ke = &events[i];
      if (qioke->ke.udata == ke->udata) {
        /*
         * If this event resulted in an error,
         * resolve the operation with said error.
         */
        if (ke->flags & EV_ERROR) {
          result = -ke->data;
          /* Our event failed for some reason. Bubble up the error */
          resolve_qio_kevent(qioke, result);
          continue;
        }

        /*
         * In this case we have no error, and our event has arrived
         * for our FD. We try to do our non-blocking
         * read/write/send/recv/accept now.
         */
        switch (qioke->op) {
        case QIO_KQ_READ: {
          result = read(qioke->ke.ident, qioke->read.buf, qioke->read.n);
          goto next;
        }
        case QIO_KQ_WRITE: {
          result = 0;
          const uint8_t *buf = qioke->write.buf;
          uint32_t len = qioke->write.n;

          for (;;) {
            int bytes = write(qioke->ke.ident, buf, len);

            if (bytes < 0) {
              result = bytes;
              break;
            }

            result += bytes;
            len -= bytes;
            buf += bytes;

            if (len <= 0)
              break;
          }

          if (result > 0)
            fsync(qioke->ke.ident);

          goto next;
        }
        case QIO_KQ_ACCEPT: {
          result = accept(qioke->ke.ident,
                          (struct sockaddr *)&qioke->accept.addr_out->addr,
                          &qioke->accept.addr_out->len);
          goto next;
        }
        case QIO_KQ_SEND: {
          result = send(qioke->ke.ident, qioke->send.buf, qioke->send.n, 0);
          goto next;
        }
        case QIO_KQ_RECV: {
          result = recv(qioke->ke.ident, qioke->recv.buf, qioke->recv.n, 0);
          goto next;
        }
        default:
          assert(false && "Matched Kevent for invalid operation");
          return;
        }
      }
    }

    /* Our pending operation found no matching event. */
    /* Re-queue it into the pending_ops vector. */
    v_kevent_push(&_qio_pending_ops, *qioke);
    continue;

  next:
    /* We ran our operation. Re-queue if necessary. */
    if (result < 0 && errno == EAGAIN || errno == EWOULDBLOCK)
      v_kevent_push(&_qio_pending_ops, *qioke);
    else if (result < 0 && errno == EINTR)
      resolve_qio_kevent(qioke, 0);
    else
      resolve_qio_kevent(qioke, result);
  }

  v_kevent_destroy(&pending);
}

/**
 * Flush up to nevents from the pending operation vector.
 *
 * Some operations can be immediately processed here:
 *  - OPENAT
 *  - SOCKET
 *  - CONNECT
 *  - CLOSE
 *
 * Other operations need to wait for an FD to be readable or writable.
 * In this case, we queue a kevent.
 *
 * These events will be processed by a sister function. If an event
 * is successful, we try to complete the operation.
 *
 * */
int flush_pending(struct kevent *events, int nevents) {
  v_kevent pending;

  // Drain the pending ops into our local vector.
  // This atomically empties the pending_ops vector,
  // Allowing other threads to queue more pending operations
  // while this vector is processed.
  v_kevent_drain(&_qio_pending_ops, &pending);

  size_t new_events = 0;

  for (size_t i = 0; i < pending.len; i++) {
    // We can safely index data here, as no other code or thread
    // has access to this pending vector.
    struct qio_kevent *ke = &pending.data[i];

    switch (ke->op) {
    case QIO_KQ_OPENAT: {
      // Perform the `openat` syscall.
      int fd = openat(ke->ke.ident, ke->openat.path,
                      O_RDWR | O_CREAT | O_APPEND | O_NONBLOCK, 0666);

      resolve_qio_kevent(ke, fd);
      continue;
    }
    case QIO_KQ_STAT: {
      int res = fstat(ke->ke.ident, ke->stat.buf);

      if (res)
        resolve_qio_kevent(ke, -errno);
      else
        resolve_qio_kevent(ke, res);

      continue;
    }
    case QIO_KQ_SOCKET: {
      // Perform the `socket` syscall.
      int fd = socket(AF_INET6, ke->socket.type, 0);
      if (fd < 0) {
        resolve_qio_kevent(ke, fd);
        continue;
      }

      int res = fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | O_NONBLOCK);
      if (res < 0) {
        resolve_qio_kevent(ke, res);
        continue;
      }

      resolve_qio_kevent(ke, fd);
      continue;
    }
    case QIO_KQ_CLOSE: {
      int res = close(ke->ke.ident);
      resolve_qio_kevent(ke, res);
      continue;
    }
    case QIO_KQ_SHUTDOWN: {
      int res = shutdown(ke->ke.ident, ke->shutdown.how);
      resolve_qio_kevent(ke, res);
      continue;
    }
    case QIO_KQ_LISTEN: {
      int res = listen(ke->ke.ident, ke->listen.backlog);
      resolve_qio_kevent(ke, res);
      continue;
    }
    case QIO_KQ_BIND: {
      int res = bind(ke->ke.ident, (void *)&ke->bind.addr, ke->bind.addr->len);
      resolve_qio_kevent(ke, res);
      continue;
    }
    case QIO_KQ_CONNECT: {
      int res = connect(ke->ke.ident, (void *)&ke->connect.addr->addr,
                        ke->connect.addr->len);
      resolve_qio_kevent(ke, res);
      continue;
    }
    case QIO_KQ_SEND:
    case QIO_KQ_RECV:
    case QIO_KQ_ACCEPT:
    case QIO_KQ_WRITE:
    case QIO_KQ_READ: {
      assert(new_events < nevents);
      if (new_events >= nevents)
        continue;

      // Copy the event into this iterations list.
      memcpy(events + new_events++, &ke->ke, sizeof(struct kevent));
      v_kevent_push(&_qio_pending_ops, *ke);
      continue;
    }
    }
  }

  v_kevent_destroy(&pending);
  return new_events;
};

qd_t _qio_append_kevent(struct qio_kevent *src_kevent) {
  qd_t qid = qd_next();
  assert(_qio_qds.len > 0);

  src_kevent->ke.udata = (void *)(intptr_t)qid;
  v_kevent_push(&_qio_pending_ops, *src_kevent);

  return qid;
}

QIO_API int32_t qio_loop() {
  struct timespec interval = {.tv_nsec = QIO_LOOP_INTERVAL_NS};

  while (true) {
    if (!_qio_pending_ops.len) {
      thrd_sleep(&interval, nullptr);
      continue;
    }

    struct timespec t = {0};

    struct kevent events[256];
    size_t total_events = sizeof(events) / sizeof(struct kevent);

    int nevents = flush_pending(events, total_events);

    // Poll each of these one-shot events.
    nevents = kevent(_qio_queue, events, nevents, events, total_events, &t);

    assert(nevents >= 0);
    if (nevents < 0)
      return nevents;

    resolve_polled(events, nevents);
  }
}

QIO_API qd_t qopen(const char *path) {
  return _qio_append_kevent(&(struct qio_kevent){
      .op = QIO_KQ_OPENAT,

      .ke.ident = AT_FDCWD,

      .openat.path = path,
  });
}

QIO_API qd_t qopenat(qfd_t fd, const char *path) {
  return _qio_append_kevent(&(struct qio_kevent){
      .op = QIO_KQ_OPENAT,

      .ke.ident = fd,

      .openat.path = path,
  });
};

QIO_API qd_t qread(qfd_t fd, uint64_t n, uint8_t buf[n]) {
  return _qio_append_kevent(&(struct qio_kevent){
      .op = QIO_KQ_READ,

      .ke.ident = fd,
      .ke.flags = EV_ADD | EV_ONESHOT,
      .ke.filter = EVFILT_READ,

      .read.n = n,
      .read.buf = buf,
  });
};

QIO_API qd_t qstat(qfd_t fd, struct qio_stat *stat) {
  return _qio_append_kevent(&(struct qio_kevent){
      .op = QIO_KQ_STAT,
      .ke.ident = fd,
      .stat.buf = &stat->statbuf,
  });
}

QIO_API qd_t qwrite(qfd_t fd, uint64_t n, const uint8_t buf[n]) {
  return _qio_append_kevent(&(struct qio_kevent){
      .op = QIO_KQ_WRITE,

      .ke.ident = fd,
      .ke.flags = EV_ADD | EV_ONESHOT,
      .ke.filter = EVFILT_WRITE,

      .write.n = n,
      .write.buf = buf,
  });
};

QIO_API qd_t qaccept(qfd_t fd, struct qio_addr *addr_out) {
  return _qio_append_kevent(&(struct qio_kevent){
      .op = QIO_KQ_ACCEPT,

      .ke.ident = fd,
      .ke.flags = EV_ADD | EV_ONESHOT,
      .ke.filter = EVFILT_READ,

      .accept.addr_out = addr_out,
  });
}

QIO_API qd_t qsocket(enum qsock_type type) {
  int os_type;
  switch (type) {
  case QSOCK_TCP:
    os_type = SOCK_STREAM;
    break;
  case QSOCK_UDP:
    os_type = SOCK_DGRAM;
    break;
  default:
    return -1;
  }
  return _qio_append_kevent(&(struct qio_kevent){
      .op = QIO_KQ_SOCKET,
      .socket.type = os_type,
  });
};

QIO_API qd_t qconnect(qfd_t fd, const struct qio_addr *addr) {
  return _qio_append_kevent(&(struct qio_kevent){
      .op = QIO_KQ_CONNECT,

      .ke.ident = fd,

      .connect.addr = addr,
  });
};

QIO_API qd_t qclose(qfd_t fd) {
  return _qio_append_kevent(&(struct qio_kevent){
      .op = QIO_KQ_CLOSE,

      .ke.ident = fd,
  });
};

QIO_API qd_t qshutdown(qfd_t fd) {
  return _qio_append_kevent(&(struct qio_kevent){
      .op = QIO_KQ_SHUTDOWN,

      .ke.ident = fd,

      .shutdown.how = SHUT_RDWR,
  });
}

QIO_API qd_t qsend(qfd_t fd, uint64_t n, const uint8_t buf[n]) {
  return _qio_append_kevent(&(struct qio_kevent){
      .op = QIO_KQ_SEND,

      .ke.ident = fd,
      .ke.flags = EV_ADD | EV_ONESHOT,
      .ke.filter = EVFILT_WRITE,

      .send.n = n,
      .send.buf = buf,
  });
};

QIO_API qd_t qrecv(qfd_t fd, uint64_t n, uint8_t buf[n]) {
  return _qio_append_kevent(&(struct qio_kevent){
      .op = QIO_KQ_RECV,

      .ke.ident = fd,
      .ke.flags = EV_ADD | EV_ONESHOT,
      .ke.filter = EVFILT_READ,

      .recv.n = n,
      .recv.buf = buf,
  });
};

QIO_API qd_t qlisten(qfd_t fd, uint32_t backlog) {
  return _qio_append_kevent(&(struct qio_kevent){
      .op = QIO_KQ_LISTEN,

      .ke.ident = fd,
      .ke.flags = EV_ADD | EV_ONESHOT,
      .ke.filter = EVFILT_READ,

      .listen.backlog = backlog,
  });
}

QIO_API qd_t qbind(qfd_t fd, const struct qio_addr *addr) {
  return _qio_append_kevent(&(struct qio_kevent){
      .op = QIO_KQ_RECV,

      .ke.ident = fd,
      .ke.flags = EV_ADD | EV_ONESHOT,
      .ke.filter = EVFILT_READ,

      .bind.addr = addr,
  });
}
#elifdef QIO_WINDOWS

#include <sys/stat.h>
#include <sys/types.h>
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

struct qiocpe_ov {
  OVERLAPPED ov;
  qd_t qd;
};

struct qio_cpevent {
  qd_t qd;
  struct qiocpe_ov *ov;

  enum {
    QIO_CP_OPENAT,
    QIO_CP_READ,
    QIO_CP_WRITE,
    QIO_CP_SOCKET,
    QIO_CP_ACCEPT,
    QIO_CP_STAT,
    QIO_CP_LISTEN,
    QIO_CP_BIND,
    QIO_CP_CONNECT,
    QIO_CP_CLOSE,
    QIO_CP_SHUTDOWN,
    QIO_CP_SEND,
    QIO_CP_RECV,
  } op;

  union {
    struct {
      qfd_t fd;
      FILE_BASIC_INFO *buf_basic;
      FILE_STANDARD_INFO *buf_standard;
    } stat;

    struct {
      const char *path;
      qfd_t root;
    } openat;

    struct {
      qfd_t fd;
      uint64_t n;
      uint8_t *buf;
    } read;

    struct {
      qfd_t fd;
      uint64_t n;
      const uint8_t *buf;
    } write;

    struct {
      int type;
    } socket;

    struct {
      qfd_t fd;
      struct qio_addr *addr_out;
    } accept;

    struct {
      qfd_t fd;
      const struct qio_addr *addr;
    } connect;

    struct {
      qfd_t fd;
      const struct qio_addr *addr;
    } bind;

    struct {
      qfd_t fd;
      uint32_t backlog;
    } listen;

    struct {
      qfd_t fd;
    } close;

    struct {
      qfd_t fd;
      int32_t how;
    } shutdown;

    struct {
      qfd_t fd;
      uint64_t n;
      const uint8_t *buf;
    } send;

    struct {
      qfd_t fd;
      uint64_t n;
      uint8_t *buf;
    } recv;
  };
};

#define T struct qio_cpevent
#define NAME cpevent
#define V_CONCURRENT
#include "vector.h"

#define QFD_INVALID_HANDLE ((uintptr_t)INVALID_HANDLE_VALUE)

v_cpevent _qio_pending_ops;

qfd_t _qio_completion_port;

QIO_API int32_t qio_init(uint64_t size) {
  v_qd_create(&_qio_qds, QIO_INTERNAL_QUEUE_INITIAL_LEN);
  mtx_init(&_qio_freelist_mtx, mtx_plain);

  v_cpevent_create(&_qio_pending_ops, QIO_INTERNAL_QUEUE_INITIAL_LEN);

  WORD wVersionRequested;
  WSADATA wsaData;
  int err;

  /* Use the MAKEWORD(lowbyte, highbyte) macro declared in Windef.h */
  wVersionRequested = MAKEWORD(2, 2);

  err = WSAStartup(wVersionRequested, &wsaData);
  if (err != 0)
    return err;

  if (LOBYTE(wsaData.wVersion) != 2 || HIBYTE(wsaData.wVersion) != 2) {
    /* Tell the user that we could not find a usable */
    /* WinSock DLL.                                  */
    printf("Could not find a usable version of Winsock.dll\n");
    WSACleanup();
    return 1;
  }

  // Create an IO completion port without associating it with a file handle.
  _qio_completion_port =
      (uintptr_t)CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);

  if (_qio_completion_port == QFD_INVALID_HANDLE)
    return 1;

  return 0;
}

void resolve_qio_cpov_event(struct qiocpe_ov *ov, int64_t res) {
  qd_t qd = ov->qd;

  struct qio_op_t op = v_qd_val_at(&_qio_qds, qd);
  assert(!op.done);
  assert(!op.result);

  free(ov);

  v_qd_set(&_qio_qds, qd, (struct qio_op_t){.done = true, .result = res});
}

/**
 * Finish the qd associated with the given completionkey.
 */
void resolve_qio_cpevent(struct qio_cpevent *cpe, int64_t res) {
  qd_t qd = cpe->qd;

  struct qio_op_t op = v_qd_val_at(&_qio_qds, qd);
  assert(!op.done);
  assert(!op.result);

  v_qd_set(&_qio_qds, qd, (struct qio_op_t){.done = true, .result = res});
}

void resolve_polled(LPOVERLAPPED_ENTRY events, ULONG nevents) {
  for (int i = 0; i < nevents; i++) {
    OVERLAPPED_ENTRY *ole = &events[i];
    struct qiocpe_ov *ov = ole->lpOverlapped;
    qd_t qd = ov->qd;

    resolve_qio_cpov_event(ov, ole->dwNumberOfBytesTransferred);
  }
}

struct qio_addr {
  struct sockaddr_in6 addr;
  socklen_t len;
};

QIO_API int qio_addrfrom(const char *restrict src, uint16_t port,
                         struct qio_addr *dst) {
  struct addrinfo *addrinfo;

  struct addrinfo hints = {.ai_family = AF_INET6};
  // TODO: Maybe this needs to include AI_PASSIVE inorder to work for bind?
  // struct addrinfo hints = {.ai_family = AF_INET6, .ai_flags = AI_PASSIVE};

  int s = getaddrinfo(src, nullptr, &hints, &addrinfo);
  if (s != 0)
    return s;

  if (addrinfo == nullptr)
    return -1;

  struct sockaddr_in6 *saddr = (struct sockaddr_in6 *)addrinfo->ai_addr;

  assert(addrinfo->ai_addrlen <= sizeof(dst->addr));
  memcpy(&dst->addr, addrinfo->ai_addr, addrinfo->ai_addrlen);

  dst->len = addrinfo->ai_addrlen;
  dst->addr.sin6_port = htons(port);

  return freeaddrinfo(addrinfo), 0;
}

struct qio_stat {
  FILE_BASIC_INFO buf_basic;
  FILE_STANDARD_INFO buf_standard;
};

uint64_t qio_statsize(struct qio_stat *stat) {
  return stat->buf_standard.EndOfFile.QuadPart;
}

/**
 * Flush up to nevents from the pending operation vector.
 *
 * Some operations can be immediately processed here:
 *  - OPENAT
 *  - SOCKET
 *  - CONNECT
 *  - CLOSE
 *
 * Other operations need to wait for an FD to be readable or writable.
 * In this case, we queue a kevent.
 *
 * These events will be processed by a sister function. If an event
 * is successful, we try to complete the operation.
 *
 * Each Async operation which needs it allocates its own OVERLAPPED struct.
 *
 * This is because the address of the struct has to be stable, and the *array
 * the struct is in* might be resized at any point.
 *
 * */
void flush_pending() {
  v_cpevent pending;

  // Drain the pending ops into our local vector.
  // This atomically empties the pending_ops vector,
  // Allowing other threads to queue more pending operations
  // while this vector is processed.
  v_cpevent_drain(&_qio_pending_ops, &pending);

  for (size_t i = 0; i < pending.len; i++) {
    // We can safely index data here, as no other code or thread
    // has access to this pending vector.
    struct qio_cpevent *cpe = &pending.data[i];

    switch (cpe->op) {
    case QIO_CP_OPENAT: {
      // TODO: Properly use the handle in openat.handle as a parent directory
      // to mimic the behavior of openat
      // Might need to use NtCreateFile instead
      HANDLE fd = CreateFileA(cpe->openat.path, GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                              OPEN_ALWAYS, FILE_FLAG_OVERLAPPED, NULL);

      if (fd == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        assert(err > 0);
        resolve_qio_cpevent(cpe, -err);
        continue;
      }

      // Assert the handle fits in a 64-bit int.
      // Sanity check here.
      assert((uintptr_t)fd < INT64_MAX);

      qfd_t qfd = (int64_t)(uintptr_t)fd;

      // Associate this handle with the completion port.
      HANDLE res = CreateIoCompletionPort(
          fd, (HANDLE)(uintptr_t)_qio_completion_port, qfd, 0);

      assert(res == _qio_completion_port);

      resolve_qio_cpevent(cpe, qfd);
      continue;
    }
    case QIO_CP_STAT: {
      BOOL res = GetFileInformationByHandleEx(
          (HANDLE)(uintptr_t)cpe->stat.fd, FileStandardInfo,
          cpe->stat.buf_standard, sizeof(FILE_STANDARD_INFO));

      if (!res) {
        resolve_qio_cpevent(cpe, -GetLastError());
        continue;
      }

      res = GetFileInformationByHandleEx((HANDLE)(uintptr_t)cpe->stat.fd,
                                         FileBasicInfo, cpe->stat.buf_basic,
                                         sizeof(FILE_BASIC_INFO));

      if (!res) {
        resolve_qio_cpevent(cpe, -GetLastError());
        continue;
      }

      resolve_qio_cpevent(cpe, 0);
      continue;
    }
    case QIO_CP_SOCKET: {
      SOCKET fd = socket(AF_INET6, cpe->socket.type, 0);

      if (fd == INVALID_SOCKET) {
        resolve_qio_cpevent(cpe, -WSAGetLastError());
        continue;
      }

      // Another sanity check
      assert((uintptr_t)fd < INT64_MAX);

      qfd_t qfd = (int64_t)(uintptr_t)fd;

      HANDLE res = CreateIoCompletionPort(
          (HANDLE)fd, (HANDLE)(uintptr_t)_qio_completion_port, qfd, 0);

      assert(res == _qio_completion_port);

      // Change the IO mode of the socket to non-blocking
      u_long iMode = 1;
      int iResult = ioctlsocket(fd, FIONBIO, &iMode);
      if (iResult != NO_ERROR)
        resolve_qio_cpevent(cpe, -iResult);

      resolve_qio_cpevent(cpe, qfd);
      continue;
    }
    case QIO_CP_CLOSE: {
      int res = closesocket(cpe->close.fd);

      if (res == SOCKET_ERROR)
        resolve_qio_cpevent(cpe, -WSAGetLastError());
      else
        resolve_qio_cpevent(cpe, res);

      continue;
    }
    case QIO_CP_SHUTDOWN: {
      int res = shutdown(cpe->shutdown.fd, cpe->shutdown.how);

      if (res == SOCKET_ERROR)
        resolve_qio_cpevent(cpe, -WSAGetLastError());
      else
        resolve_qio_cpevent(cpe, res);

      continue;
    }
    case QIO_CP_LISTEN: {
      int res = listen(cpe->listen.fd, cpe->listen.backlog);

      if (res == SOCKET_ERROR)
        resolve_qio_cpevent(cpe, -WSAGetLastError());
      else
        resolve_qio_cpevent(cpe, res);

      continue;
    }
    case QIO_CP_BIND: {
      int res = bind(cpe->bind.fd, (void *)&cpe->bind.addr->addr,
                     cpe->bind.addr->len);

      if (res == SOCKET_ERROR)
        resolve_qio_cpevent(cpe, -WSAGetLastError());
      else
        resolve_qio_cpevent(cpe, res);

      continue;
    }
    case QIO_CP_READ: {
      // Allocate a new overlapped structure.
      cpe->ov = calloc(sizeof(struct qiocpe_ov), 1);
      assert(cpe->ov != NULL);

      cpe->ov->qd = cpe->qd;

      // Queue up the read.
      bool res = ReadFile((HANDLE)(uintptr_t)cpe->read.fd, cpe->read.buf,
                          cpe->read.n, NULL, &cpe->ov->ov);

      // An asynchronous read always returns false.
      assert(!res);

      // We must check get last error for ERROR_IO_PENDING
      DWORD err = GetLastError();

      if (err != ERROR_IO_PENDING)
        resolve_qio_cpevent(cpe, -err);
      else
        v_cpevent_push(&_qio_pending_ops, *cpe);

      continue;
    }
    case QIO_CP_WRITE: {
      // Allocate a new overlapped structure.
      cpe->ov = calloc(sizeof(struct qiocpe_ov), 1);
      assert(cpe->ov != NULL);

      cpe->ov->qd = cpe->qd;

      // Queue up the read.
      bool res = WriteFile((HANDLE)(uintptr_t)cpe->read.fd, cpe->read.buf,
                           cpe->read.n, NULL, &cpe->ov->ov);

      // An asynchronous write always returns false.
      assert(!res);

      // We must check get last error for ERROR_IO_PENDING
      DWORD err = GetLastError();

      if (err != ERROR_IO_PENDING)
        resolve_qio_cpevent(cpe, -err);
      else
        v_cpevent_push(&_qio_pending_ops, *cpe);

      continue;
    }
    case QIO_CP_SEND: {
      // Allocate a new overlapped structure.
      cpe->ov = calloc(sizeof(struct qiocpe_ov), 1);
      assert(cpe->ov != NULL);

      cpe->ov->qd = cpe->qd;

      WSABUF buf = {.len = cpe->send.n, (char *)cpe->send.buf};

      DWORD flags = 0, bytes = 0;
      int res =
          WSASend(cpe->send.fd, &buf, 1, &bytes, flags, &cpe->ov->ov, NULL);

      DWORD err = res == SOCKET_ERROR ? WSAGetLastError() : 0;

      if (res != SOCKET_ERROR)
        continue;
      else if (err == WSAEWOULDBLOCK)
        v_cpevent_push(&_qio_pending_ops, *cpe);
      else if (err != WSA_IO_PENDING)
        resolve_qio_cpevent(cpe, -err);
      else
        assert(res < 0 && err == WSA_IO_PENDING);

      break;
    }
    case QIO_CP_RECV: {
      // Allocate a new overlapped structure.
      cpe->ov = calloc(sizeof(struct qiocpe_ov), 1);
      assert(cpe->ov != NULL);

      cpe->ov->qd = cpe->qd;

      WSABUF buf = {.len = cpe->recv.n, (char *)cpe->recv.buf};

      DWORD flags = 0, bytes = 0;
      int res =
          WSARecv(cpe->recv.fd, &buf, 1, &bytes, &flags, &cpe->ov->ov, nullptr);

      DWORD err = res == SOCKET_ERROR ? WSAGetLastError() : 0;

      /*
       * Some issue I can see is when events happen that aren't on the
       * completion port. Recvs don't seem to be triggering OV events.
       */
      if (res != SOCKET_ERROR)
        continue;
      else if (err == WSAEWOULDBLOCK)
        v_cpevent_push(&_qio_pending_ops, *cpe);
      else if (err != WSA_IO_PENDING)
        resolve_qio_cpevent(cpe, -err);
      else
        assert(res < 0 && err == WSA_IO_PENDING);

      break;
    }
    case QIO_CP_CONNECT: {
      int result = connect(cpe->connect.fd, (void *)&cpe->connect.addr->addr,
                           cpe->connect.addr->len);

      int err = 0;
      if (result == SOCKET_ERROR)
        err = WSAGetLastError();

      switch (err) {
        /* Retry until we get a real error */
      case WSAEINVAL:
      case WSAEWOULDBLOCK:
      case WSAEALREADY:
        v_cpevent_push(&_qio_pending_ops, *cpe);
        continue;
        /* Already connected */
        /* TODO:
         * This isn't a proper way to check if our connect was successful.
         * Consecutive qconnect calls *wont* error like we'd expect.
         */
      case WSAEISCONN:
        resolve_qio_cpevent(cpe, 0);
        continue;
        /* shouldn't really happen */
      case 0:
        resolve_qio_cpevent(cpe, result);
        continue;
        /* a real error */
      default:
        resolve_qio_cpevent(cpe, -err);
        continue;
      }

      if (err > 0 && err == WSAEWOULDBLOCK)
        v_cpevent_push(&_qio_pending_ops, *cpe);
      else if (err > 0)
        resolve_qio_cpevent(cpe, -err);
      else
        resolve_qio_cpevent(cpe, result);

      continue;
    }
    case QIO_CP_ACCEPT:
      SOCKET fd =
          accept(cpe->accept.fd, (struct sockaddr *)&cpe->accept.addr_out->addr,
                 &cpe->accept.addr_out->len);

      int err = 0;

      if (fd == INVALID_SOCKET)
        err = WSAGetLastError();

      if (err == WSAEWOULDBLOCK)
        v_cpevent_push(&_qio_pending_ops, *cpe);
      else if (err)
        resolve_qio_cpevent(cpe, -err);

      if (err)
        continue;

      assert((uintptr_t)fd < INT64_MAX);
      qfd_t qfd = (int64_t)(uintptr_t)fd;

      HANDLE res = CreateIoCompletionPort(
          (HANDLE)fd, (HANDLE)(uintptr_t)_qio_completion_port, qfd, 0);

      assert(res == _qio_completion_port);

      // Change the IO mode of the socket to non-blocking
      u_long iMode = 1;
      int iResult = ioctlsocket(fd, FIONBIO, &iMode);
      if (iResult != NO_ERROR)
        resolve_qio_cpevent(cpe, -iResult);

      resolve_qio_cpevent(cpe, fd);

      continue;
    }
  }

  v_cpevent_destroy(&pending);
};

QIO_API void qio_destroy() {}

qd_t _qio_append_cpevent(struct qio_cpevent *src_cpevent) {
  qd_t qid = qd_next();
  assert(_qio_qds.len > 0);

  src_cpevent->qd = qid;
  v_cpevent_push(&_qio_pending_ops, *src_cpevent);

  return qid;
}

QIO_API int32_t qio_loop() {
  struct timespec interval = {.tv_nsec = QIO_LOOP_INTERVAL_NS};

  while (true) {
    if (!_qio_pending_ops.len) {
      thrd_sleep(&interval, nullptr);
      goto cq;
    }

    struct timespec t = {0};

    OVERLAPPED_ENTRY events[256];
    size_t total_events = sizeof(events) / sizeof(OVERLAPPED_ENTRY);

    flush_pending();

  cq:
    ULONG nevents = 0;

    // Pull up to total_events IO events off the port.
    bool res = GetQueuedCompletionStatusEx(
        (HANDLE)(uintptr_t)_qio_completion_port, (LPOVERLAPPED_ENTRY)&events,
        total_events, &nevents,
        0,    // Timeout - 0 to return immediately.
        false // Not alertable.
    );

    assert(nevents >= 0);
    if (nevents < 0)
      return nevents;

    if (res)
      resolve_polled(events, nevents);
  }
}

QIO_API qd_t qopen(const char *path) {
  return _qio_append_cpevent(&(struct qio_cpevent){
      .op = QIO_CP_OPENAT,
      .openat.root = QFD_INVALID_HANDLE,
      .openat.path = path,
  });
}

QIO_API qd_t qopenat(qfd_t fd, const char *path) {
  return _qio_append_cpevent(&(struct qio_cpevent){
      .op = QIO_CP_OPENAT,
      .openat.root = fd,
      .openat.path = path,
  });
}

QIO_API qd_t qread(qfd_t fd, uint64_t n, uint8_t buf[n]) {
  assert(n < UINT32_MAX);
  return _qio_append_cpevent(&(struct qio_cpevent){
      .op = QIO_CP_READ,
      .read.n = n,
      .read.buf = buf,
      .read.fd = fd,
  });
}

QIO_API qd_t qclose(qfd_t fd) {
  return _qio_append_cpevent(&(struct qio_cpevent){
      .op = QIO_CP_CLOSE,
      .close.fd = fd,
  });
}

QIO_API qd_t qshutdown(qfd_t fd) {
  return _qio_append_cpevent(&(struct qio_cpevent){
      .op = QIO_CP_SHUTDOWN,
      .shutdown.fd = fd,
      .shutdown.how = SD_BOTH,
  });
}

QIO_API qd_t qbind(qfd_t fd, const struct qio_addr *addr) {
  return _qio_append_cpevent(&(struct qio_cpevent){
      .op = QIO_CP_BIND,
      .bind.fd = fd,
      .bind.addr = addr,
  });
}

QIO_API qd_t qlisten(qfd_t fd, uint32_t backlog) {
  return _qio_append_cpevent(&(struct qio_cpevent){
      .op = QIO_CP_LISTEN,
      .listen.fd = fd,
      .listen.backlog = backlog,
  });
}

QIO_API qd_t qaccept(qfd_t fd, struct qio_addr *addr_out) {
  return _qio_append_cpevent(&(struct qio_cpevent){
      .op = QIO_CP_ACCEPT,
      .accept.fd = fd,
      .accept.addr_out = addr_out,
  });
}

QIO_API qd_t qstat(qfd_t fd, struct qio_stat *stat) {
  return _qio_append_cpevent(&(struct qio_cpevent){
      .op = QIO_CP_STAT,
      .stat.fd = fd,
      .stat.buf_basic = &stat->buf_basic,
      .stat.buf_standard = &stat->buf_standard,
  });
}

QIO_API qd_t qsocket(enum qsock_type type) {
  int os_type = 0;

  switch (type) {
  case QSOCK_TCP:
    os_type = SOCK_STREAM;
    break;
  case QSOCK_UDP:
    os_type = SOCK_DGRAM;
    break;
  default:
    return -1;
  }

  return _qio_append_cpevent(&(struct qio_cpevent){
      .op = QIO_CP_SOCKET,
      .socket.type = os_type,
  });
}

QIO_API qd_t qconnect(qfd_t fd, const struct qio_addr *addr) {
  return _qio_append_cpevent(&(struct qio_cpevent){
      .op = QIO_CP_CONNECT,
      .connect.fd = fd,
      .connect.addr = addr,
  });
}

QIO_API qd_t qsend(qfd_t fd, uint64_t n, const uint8_t buf[n]) {
  return _qio_append_cpevent(&(struct qio_cpevent){
      .op = QIO_CP_SEND,
      .send.fd = fd,
      .send.n = n,
      .send.buf = buf,
  });
}

QIO_API qd_t qrecv(qfd_t fd, uint64_t n, uint8_t buf[n]) {
  return _qio_append_cpevent(&(struct qio_cpevent){
      .op = QIO_CP_RECV,
      .recv.fd = fd,
      .recv.n = n,
      .recv.buf = buf,
  });
}

#endif
#endif
