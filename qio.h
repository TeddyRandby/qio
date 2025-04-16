#ifndef QIO_H
#define QIO_H

#include <assert.h>
#include <stdint.h>
#include <threads.h>
#include <time.h>

#define QIO_API static inline

#ifdef QIO_LINUX

#include <fcntl.h>
#include <linux/io_uring.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

/*
 * Abstraction over os file/pipe/console/socket types.
 */
typedef int qfd_t;

#elifdef QIO_MACOS

#include <fcntl.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <unistd.h>

typedef int qfd_t;

#elifdef QIO_WINDOWS

#include <windows.h>
/* Implemented using IO Completion Ports */

typedef HANDLE qfd_t;
#else
typedef uint64_t qfd_t;

#endif

/*
 * A qid (pronounced 'kid') is a handle representing one IO operation.
 * It is used to:
 *  - Check on the status of its corresponding operation.
 *  - Get the result of its operation
 */
typedef int64_t qd_t;

/*
 * Initialize QIO. This should only be called *once* per thread.
 *
 * This sets up platform-specific IO datastructures. (Like the queues in
 * io_uring).
 */
QIO_API int32_t qio_init(uint64_t size);
QIO_API int32_t qio_loop();
QIO_API void qio_destroy();

/*
 * The following are the 'queued' versions of corresponding POSIX functions.
 */
QIO_API qd_t qopen(const char *path);
QIO_API qd_t qopenat(qfd_t fd, const char *path);

QIO_API qd_t qread(qfd_t fd, uint64_t n, uint8_t buf[n]);
QIO_API qd_t qwrite(qfd_t fd, uint64_t n, uint8_t buf[n]);

QIO_API qd_t qsocket(int32_t domain, int32_t type, int32_t protocol);
QIO_API qd_t qlisten(qfd_t fd, int32_t backlog);
QIO_API qd_t qaccept(qfd_t fd, void *addr, void *addrlen, uint32_t flags);
QIO_API qd_t qbind(qfd_t fd, void *addr, uint64_t addrlen);
QIO_API qd_t qconnect(qfd_t fd, void *addr, uint64_t addrlen);
QIO_API qd_t qclose(qfd_t fd);
QIO_API qd_t qshutdown(qfd_t fd, int32_t how);

QIO_API qd_t qsend(qfd_t fd, uint64_t n, uint8_t buf[n]);
QIO_API qd_t qrecv(qfd_t fd, uint64_t n, uint8_t buf[n]);

struct qio_op_t {
  /* Has the io op completed? */
  int8_t done;
  /* OS return values */
  int64_t result;
  uint64_t flags;
  /* User Data */
  union {
    uint64_t ud;
    qd_t next_free;
  };
};

#define T struct qio_op_t
#define NAME qd
#define V_CONCURRENT
#include "vector.h"

mtx_t freelist_mtx;

/*
 * The following variable (and other platform-specific globals like it)
 * are marked static, and not _Thread_local. This is intentional -
 * it is almost *always* the case that IO operations qre queued from
 * a different thread than the one running the qio_loop. Because of this,
 * these data structures need to be thread-safe and static.
 */
static v_qd qds = {0};
static uint64_t qd_free = -1;

/*
 * This function is *not* blocking. It will immediately return:
 *  - nonzero if the corresponding operation is complete.
 *  - zero if the operation is still in progress.
 */
QIO_API int8_t qd_status(qd_t qd) {
  assert(qd < qds.len);
  return v_qd_val_at(&qds, qd).done;
}

/*
 * This operation is *blocking*. It block the caller until the qid's
 * corresponding operation is complete.
 */
QIO_API int64_t qd_result(qd_t qd) {
  assert(qd < qds.len);

  // Simply block until we have received a response from the os.
  while (!qd_status(qd))
    ;

  return v_qd_val_at(&qds, qd).result;
}

QIO_API void qd_setud(qd_t qd, uint64_t ud) {
  assert(qd < qds.len);

  struct qio_op_t v = v_qd_val_at(&qds, qd);

  v.ud = ud;

  v_qd_set(&qds, qd, v);
}

QIO_API uint64_t qd_getud(qd_t qd, uint64_t ud) {
  assert(qd < qds.len);
  return v_qd_val_at(&qds, qd).ud;
}

static uint64_t negative_one = -1;
static struct qio_op_t destroyed = {.next_free = (uint64_t)-1};

/* the O(n) operation, appending to list */
static inline void _freelist_push(qd_t qd) {
  assert(qd_free != negative_one);

  struct qio_op_t op = v_qd_val_at(&qds, qd_free);
  qd_t p_qd = qd_free;

  while (op.next_free != negative_one) {
    p_qd = op.next_free;
    op = v_qd_val_at(&qds, op.next_free);
  }

  assert(op.next_free == negative_one);
  assert(p_qd != negative_one);
  assert(p_qd < qds.len);

  op.next_free = qd;
  v_qd_set(&qds, p_qd, op);
}

/* The O(1) operation, popping from list */
static inline qd_t _freelist_pop() {
  assert(qd_free != negative_one);

  qd_t qd = qd_free;

  struct qio_op_t op = v_qd_val_at(&qds, qd_free);
  qd_free = op.next_free;

  return qd;
}

QIO_API void qd_destroy(qd_t qd) {
  assert(qd < qds.len);

  /* Block until the operation is done. */
  qd_result(qd);

  assert(v_qd_val_at(&qds, qd).done);

  mtx_lock(&freelist_mtx);

  v_qd_set(&qds, qd, destroyed);

  if (qd_free == negative_one)
    return qd_free = qd, (void)mtx_unlock(&freelist_mtx);

  _freelist_push(qd);

  mtx_unlock(&freelist_mtx);
}

QIO_API qd_t qd_next() {
  mtx_lock(&freelist_mtx);

  if (qd_free != negative_one) {
    qd_t qd = _freelist_pop();

    if (qd != negative_one)
      return mtx_unlock(&freelist_mtx), qd;
  }

  qd_t qd = v_qd_push(&qds, (struct qio_op_t){});
  assert(qds.len > 0);

  return mtx_unlock(&freelist_mtx), qd;
}

#define QIO_LOOP_INTERVAL_NS 500000

#ifdef QIO_LINUX
#define io_uring_smp_store_release(p, v)                                       \
  atomic_store_explicit((_Atomic typeof(*(p)) *)(p), (v), memory_order_release)

#define io_uring_smp_load_acquire(p)                                           \
  atomic_load_explicit((_Atomic typeof(*(p)) *)(p), memory_order_acquire)

static qfd_t ring;

static uint32_t *sring_tail, *sring_head, *sring_mask, *sring_array,
    *cring_head, *cring_tail, *cring_mask;

static struct io_uring_sqe *sqes;
static struct io_uring_cqe *cqes;
static uint32_t ring_entries;

#define T struct io_uring_sqe
#define NAME sqe
#define V_CONCURRENT
#include "vector.h"

static v_sqe queued_sqes;

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
    unsigned head = io_uring_smp_load_acquire(cring_head);

    /*
     * If we don't have any ops to queue and the receive buffer is empty:
     * Sleep and try again later.
     */
    if (!queued_sqes.len && head == *cring_tail) {
      thrd_sleep(&interval, nullptr);
      continue;
    }

    /*
     * If we have opts to queue
     */
    if (queued_sqes.len) {
      /*
       * Atomically drain all our queued sqes into a local buffer.
       */
      v_sqe buffered;
      v_sqe_drain(&queued_sqes, &buffered);

      // Rudimentary assert here.
      // It would be better to just put everything we can in ring,
      // and re-queue the rest.
      assert(buffered.len < ring_entries);

      for (int i = 0; i < buffered.len; i++) {
        struct io_uring_sqe *src_sqe = &buffered.data[i];

        unsigned index, tail;
        tail = *sring_tail;
        index = tail & *sring_mask;

        assert(src_sqe->user_data < qds.len);
        struct io_uring_sqe *dst_sqe = &sqes[index];
        memcpy(dst_sqe, src_sqe, sizeof(struct io_uring_sqe));

        sring_array[index] = index;
        tail++;

        /* Update the tail */
        io_uring_smp_store_release(sring_tail, tail);
      }

      /* System call to trigger kernel */
      if (buffered.len)
        io_uring_enter(ring, buffered.len, 0, 0, nullptr);

      /* Free our buffered list */
      v_sqe_destroy(&buffered);
    }

    /*
     * If we have ops completed
     */
    if (head != *cring_tail) {
      /* Get the entry */
      unsigned index = head & (*cring_mask);
      struct io_uring_cqe *cqe = &cqes[index];
      head++;

      qd_t qid = cqe->user_data;

      assert(qid < qds.len);
      assert(v_qd_val_at(&qds, qid).done == false);
      assert(v_qd_val_at(&qds, qid).flags == 0);
      assert(v_qd_val_at(&qds, qid).result == 0);
      /*
       * Perform this via a set for two resons:
       *  - The struct is small enough that passying and copying by value is
       * fine
       *  - This performes the update in *one atomic operation*. Holding
       * pointers into the vector is *unsafe* in a concurrent vector, as the
       * array could be reallocated out from underneath you.
       */
      v_qd_set(&qds, qid,
               (struct qio_op_t){
                   .result = cqe->res,
                   .flags = cqe->flags,
                   .done = true,
               });

      /* Write barrier so that update to the head are made visible */
      io_uring_smp_store_release(cring_head, head);
    }
  }
}

QIO_API void qio_destroy() {}

QIO_API int32_t qio_init(uint64_t size) {
  /* Initialize qds. All OS's need to do this. */
  ring_entries = size;
  v_qd_create(&qds, 64);
  mtx_init(&freelist_mtx, mtx_plain);

  struct io_uring_params p = {0};
  /* The submission and completion queue */
  void *sq, *cq;

  ring = io_uring_setup(ring_entries, &p);
  if (ring < 0)
    return -1;

  int sring_sz = p.sq_off.array + p.sq_entries * sizeof(unsigned);
  int cring_sz = p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe);

  if (p.features & IORING_FEAT_SINGLE_MMAP) {
    if (cring_sz > sring_sz)
      sring_sz = cring_sz;
    cring_sz = sring_sz;
  }
  sq = mmap(0, sring_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
            ring, IORING_OFF_SQ_RING);
  if (sq == MAP_FAILED)
    return -1;

  assert(sq != nullptr);

  if (p.features & IORING_FEAT_SINGLE_MMAP) {
    cq = sq;
  } else {
    /* Map in the completion queue ring buffer in older kernels separately */
    cq = mmap(0, cring_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
              ring, IORING_OFF_CQ_RING);
    if (cq == MAP_FAILED)
      return -1;
  }

  sring_tail = (uint32_t *)((uint8_t *)sq + p.sq_off.tail);
  sring_head = (uint32_t *)((uint8_t *)sq + p.sq_off.head);
  sring_mask = (uint32_t *)((uint8_t *)sq + p.sq_off.ring_mask);
  sring_array = (uint32_t *)((uint8_t *)sq + p.sq_off.array);

  sqes = (struct io_uring_sqe *)mmap(
      0, p.sq_entries * sizeof(struct io_uring_sqe), PROT_READ | PROT_WRITE,
      MAP_SHARED | MAP_POPULATE, ring, IORING_OFF_SQES);

  if (sqes == MAP_FAILED)
    return -1;

  assert(cq != nullptr);

  cring_head = (uint32_t *)((uint8_t *)cq + p.cq_off.head);
  cring_tail = (uint32_t *)((uint8_t *)cq + p.cq_off.tail);
  cring_mask = (uint32_t *)((uint8_t *)cq + p.cq_off.ring_mask);
  cqes = (struct io_uring_cqe *)((uint8_t *)cq + p.cq_off.cqes);

  return 0;
}

qd_t append_sqe(struct io_uring_sqe *src_sqe) {
  qd_t qid = qd_next();
  assert(qds.len > 0);

  src_sqe->user_data = qid;
  v_sqe_push(&queued_sqes, *src_sqe);

  return qid;
}

qd_t qopen(const char *path) { return qopenat(AT_FDCWD, path); }

/*
 * TODO: Properly setup mode here
 *
 * Created files permissions are wonky.
 */
qd_t qopenat(qfd_t fd, const char *path) {
  return append_sqe(&(struct io_uring_sqe){
      .opcode = IORING_OP_OPENAT,
      .fd = fd,
      .addr = (uintptr_t)path,
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
  return append_sqe(&(struct io_uring_sqe){
      .opcode = IORING_OP_READ,
      .fd = fd,
      .addr = (uintptr_t)buf,
      .len = n,
  });
}

QIO_API qd_t qwrite(qfd_t fd, uint64_t n, uint8_t buf[n]) {
  assert(n < UINT32_MAX);
  return append_sqe(&(struct io_uring_sqe){
      .opcode = IORING_OP_WRITE,
      .fd = fd,
      .addr = (uintptr_t)buf,
      .len = n,
  });
}

QIO_API qd_t qsend(qfd_t fd, uint64_t n, uint8_t buf[n]) {
  assert(n < UINT32_MAX);
  return append_sqe(&(struct io_uring_sqe){
      .opcode = IORING_OP_SEND,
      .fd = fd,
      .addr = (uintptr_t)buf,
      .len = n,
  });
}

QIO_API qd_t qrecv(qfd_t fd, uint64_t n, uint8_t buf[n]) {
  return append_sqe(&(struct io_uring_sqe){
      .opcode = IORING_OP_RECV,
      .fd = fd,
      .addr = (uintptr_t)buf,
      .len = n,
  });
}

QIO_API qd_t qsocket(int domain, int type, int protocol) {
  return append_sqe(&(struct io_uring_sqe){
      .opcode = IORING_OP_SOCKET,
      .fd = domain,
      .len = protocol,
      .off = type,
  });
}

QIO_API qd_t qclose(qfd_t fd) {
  return append_sqe(&(struct io_uring_sqe){
      .opcode = IORING_OP_CLOSE,
      .fd = fd,
  });
}

QIO_API qd_t qshutdown(qfd_t fd, int32_t how) {
  return append_sqe(&(struct io_uring_sqe){
      .opcode = IORING_OP_SHUTDOWN,
      .fd = fd,
  });
}

QIO_API qd_t qaccept(qfd_t fd, void *addr, void *addrlen, uint32_t flags) {
  return append_sqe(&(struct io_uring_sqe){
      .opcode = IORING_OP_ACCEPT,
      .fd = fd,
      .accept_flags = flags,
      .addr = (uintptr_t)addr,
      .off = (uintptr_t)addrlen,
  });
}

QIO_API qd_t qconnect(qfd_t fd, void *addr, uint64_t addrlen) {
  return append_sqe(&(struct io_uring_sqe){
      .opcode = IORING_OP_CONNECT,
      .fd = fd,
      .addr = (uintptr_t)addr,
      .off = addrlen,
  });
}

QIO_API qd_t qlisten(qfd_t fd, int32_t backlog) { return -1; }

QIO_API qd_t qbind(qfd_t fd, void *addr, uint64_t addrlen) { return -1; }

// When kernel version updates
/*
QIO_API qd_t qlisten(qfd_t fd, int32_t backlog) {
  return append_sqe(&(struct io_uring_sqe){
    .opcode = IORING_OP_LISTEN,
    .fd = fd,
    .len = backlog,
  });
}

QIO_API qd_t qbind(qfd_t fd, void *addr, uint64_t addrlen) {
  return append_sqe(&(struct io_uring_sqe){
    .opcode = IORING_OP_BIND,
    .fd = fd,
    .addr = addr,
    .off = addrlen,
  });
}
*/

#elifdef QIO_MACOS

/*
 * This impl needs a lot of work - it isn't functional.
 * kqueue doesn't perform any operations - it just waits on events.
 *
 * This implementation needs to actually *perform*
 * reads, writes, accepts, sends, recvs, and the like.
 *
 * Maybe the best thing to do is to store this data in the kevent buffer.
 *
 * During QIO loop, we try and perform IO on for all events that we
 * received as ready. If we receive EAGAIN, we queue it again
 * to try later.
 *
 * This leaves all IO going through a single thread, and allows us
 * to make synthetic async events for operations that will just
 * wrap blocking syscalls (open, openat, socket, listen)
 */

qfd_t queue;
_Atomic int64_t waiting = 0;

struct qio_kevent {
  struct kevent ke;

  enum {
    QIO_KQ_OPENAT,
    QIO_KQ_READ,
    QIO_KQ_WRITE,
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
      const char *path;
    } openat;

    struct {
      uint64_t n;
      uint8_t *buf;
    } read;

    struct {
      uint64_t n;
      uint8_t *buf;
    } write;

    struct {
      int32_t domain, protocol, type;
    } socket;

    struct {
      void *addr, *addrlen;
      uint32_t flags;
    } accept;

    struct {
      void *addr;
      uint64_t addrlen;
    } connect;

    struct {
      void *addr;
      uint64_t addrlen;
    } bind;

    struct {
      uint64_t backlog;
    } listen;

    struct {
    } close;

    struct {
      int32_t how;
    } shutdown;

    struct {
      uint64_t n;
      uint8_t *buf;
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

v_kevent pending_ops;

QIO_API void qio_destroy() {}

int setfd_nonblock(int fd) {
  int flags = fcntl(fd, F_GETFL);
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

QIO_API int32_t qio_init(uint64_t size) {
  v_kevent_create(&pending_ops, 64);

  int res;
  if ((res = setfd_nonblock(STDIN_FILENO)) < 0)
    return res;

  if ((res = setfd_nonblock(STDOUT_FILENO)) < 0)
    return res;

  if ((res = setfd_nonblock(STDERR_FILENO)) < 0)
    return res;

  queue = kqueue();

  if (queue < 0)
    return queue;

  return 0;
}

/**
 * Finish the qd stored in ke->ke.udata with res and flags;
 */
void resolve_qio_kevent(struct qio_kevent *ke, int64_t res, int64_t flags) {
  qd_t qd = (uintptr_t)ke->ke.udata;
  struct qio_op_t op = v_qd_val_at(&qds, qd);

  op.result = res < 0 ? -errno : res;
  op.flags = flags;
  op.done = true;

  v_qd_set(&qds, qd, op);
}

void resolve_polled(struct kevent *events, int nevents) {
  v_kevent pending;

  // Drain the pending ops into our local vector.
  // This atomically empties the pending_ops vector,
  // Allowing other threads to queue more pending operations
  // while this vector is processed.
  v_kevent_drain(&pending_ops, &pending);

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
          resolve_qio_kevent(qioke, result, 0);
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
          result = write(qioke->ke.ident, qioke->write.buf, qioke->write.n);

          if (result > 0)
            fsync(qioke->ke.ident);
          goto next;
        }
        case QIO_KQ_ACCEPT: {
          result = accept(qioke->ke.ident, qioke->accept.addr,
                          qioke->accept.addrlen);
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
    v_kevent_push(&pending_ops, *qioke);
    continue;

  next:
    /* We ran our operation. Re-queue if necessary. */
    if (result < 0 && errno == EAGAIN || errno == EWOULDBLOCK)
      v_kevent_push(&pending_ops, *qioke);
    else
      resolve_qio_kevent(qioke, result, 0);
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
  v_kevent_drain(&pending_ops, &pending);

  size_t new_events = 0;

  for (size_t i = 0; i < pending.len; i++) {
    // We can safely index data here, as no other code or thread
    // has access to this pending vector.
    struct qio_kevent *ke = &pending.data[i];

    switch (ke->op) {
    case QIO_KQ_OPENAT: {
      // Perform the `openat` syscall.
      int fd = openat(ke->ke.ident, ke->openat.path,
                      O_RDWR | O_CREAT | O_APPEND | O_NONBLOCK);

      resolve_qio_kevent(ke, fd, 0);
      continue;
    }
    case QIO_KQ_SOCKET: {
      // Perform the `socket` syscall.
      int fd = socket(ke->socket.domain, ke->socket.type, ke->socket.protocol);
      if (fd < 0) {
        resolve_qio_kevent(ke, fd, 0);
        continue;
      }

      int res = fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | O_NONBLOCK);
      if (res < 0) {
        resolve_qio_kevent(ke, res, 0);
        continue;
      }

      resolve_qio_kevent(ke, fd, 0);
      continue;
    }
    case QIO_KQ_CONNECT: {
      int res = connect(ke->ke.ident, ke->connect.addr, ke->connect.addrlen);
      resolve_qio_kevent(ke, res, 0);
      continue;
    }
    case QIO_KQ_CLOSE: {
      int res = close(ke->ke.ident);
      resolve_qio_kevent(ke, res, 0);
      continue;
    }
    case QIO_KQ_SHUTDOWN: {
      int res = shutdown(ke->ke.ident, ke->shutdown.how);
      resolve_qio_kevent(ke, res, 0);
      continue;
    }
    case QIO_KQ_LISTEN: {
      int res = listen(ke->ke.ident, ke->listen.backlog);
      resolve_qio_kevent(ke, res, 0);
      continue;
    }
    case QIO_KQ_BIND: {
      int res = bind(ke->ke.ident, ke->bind.addr, ke->bind.addrlen);
      resolve_qio_kevent(ke, res, 0);
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
      v_kevent_push(&pending_ops, *ke);
      continue;
    }
    }
  }

  v_kevent_destroy(&pending);
  return new_events;
};

qd_t append_kevent(struct qio_kevent *src_kevent) {
  qd_t qid = qd_next();
  assert(qds.len > 0);

  src_kevent->ke.udata = (void *)(intptr_t)qid;
  v_kevent_push(&pending_ops, *src_kevent);

  return qid;
}

QIO_API int32_t qio_loop() {
  struct timespec interval = {.tv_nsec = QIO_LOOP_INTERVAL_NS};

  while (true) {
    // if (!pending_ops.len) {
    //   thrd_sleep(&interval, nullptr);
    //   continue;
    // }

    struct timespec t = {0};

    struct kevent events[256];
    size_t total_events = sizeof(events) / sizeof(struct kevent);

    int nevents = flush_pending(events, total_events);

    // Poll each of these one-shot events.
    nevents = kevent(queue, events, nevents, events, total_events, &t);

    assert(nevents >= 0);
    if (nevents < 0)
      return nevents;

    resolve_polled(events, nevents);
  }
}

QIO_API qd_t qopen(const char *path) {
  return append_kevent(&(struct qio_kevent){
      .op = QIO_KQ_OPENAT,

      .ke.ident = AT_FDCWD,

      .openat.path = path,
  });
}

QIO_API qd_t qopenat(qfd_t fd, const char *path) { return -1; };

QIO_API qd_t qread(qfd_t fd, uint64_t n, uint8_t buf[n]) {
  return append_kevent(&(struct qio_kevent){
      .op = QIO_KQ_READ,

      .ke.ident = fd,
      .ke.flags = EV_ADD | EV_ONESHOT,
      .ke.filter = EVFILT_READ,

      .read.n = n,
      .read.buf = buf,
  });
};

QIO_API qd_t qwrite(qfd_t fd, uint64_t n, uint8_t buf[n]) {
  return append_kevent(&(struct qio_kevent){
      .op = QIO_KQ_WRITE,

      .ke.ident = fd,
      .ke.flags = EV_ADD | EV_ONESHOT,
      .ke.filter = EVFILT_WRITE,

      .write.n = n,
      .write.buf = buf,
  });
};

QIO_API qd_t qaccept(qfd_t fd, void *addr, void *addrlen, uint32_t flags) {
  return append_kevent(&(struct qio_kevent){
      .op = QIO_KQ_ACCEPT,

      .ke.ident = fd,
      .ke.flags = EV_ADD | EV_ONESHOT,
      .ke.filter = EVFILT_READ,

      .accept.addr = addr,
      .accept.addrlen = addrlen,
      .accept.flags = flags,
  });
}

QIO_API qd_t qsocket(int domain, int type, int protocol) {
  return append_kevent(&(struct qio_kevent){
      .op = QIO_KQ_SOCKET,

      .socket.domain = domain,
      .socket.protocol = protocol,
      .socket.type = type,
  });
};

QIO_API qd_t qconnect(qfd_t fd, void *addr, uint64_t addrlen) {
  return append_kevent(&(struct qio_kevent){
      .op = QIO_KQ_CONNECT,

      .ke.ident = fd,

      .connect.addr = addr,
      .connect.addrlen = addrlen,
  });
};

QIO_API qd_t qclose(qfd_t fd) {
  return append_kevent(&(struct qio_kevent){
      .op = QIO_KQ_CLOSE,

      .ke.ident = fd,
  });
};

QIO_API qd_t qshutdown(qfd_t fd, int32_t how) {
  return append_kevent(&(struct qio_kevent){
      .op = QIO_KQ_SHUTDOWN,

      .ke.ident = fd,

      .shutdown.how = how,
  });
}

QIO_API qd_t qsend(qfd_t fd, uint64_t n, uint8_t buf[n]) {
  return append_kevent(&(struct qio_kevent){
      .op = QIO_KQ_SEND,

      .ke.ident = fd,
      .ke.flags = EV_ADD | EV_ONESHOT,
      .ke.filter = EVFILT_WRITE,

      .send.n = n,
      .send.buf = buf,
  });
};

QIO_API qd_t qrecv(qfd_t fd, uint64_t n, uint8_t buf[n]) {
  return append_kevent(&(struct qio_kevent){
      .op = QIO_KQ_RECV,

      .ke.ident = fd,
      .ke.flags = EV_ADD | EV_ONESHOT,
      .ke.filter = EVFILT_READ,

      .recv.n = n,
      .recv.buf = buf,
  });
};

QIO_API qd_t qlisten(qfd_t fd, int32_t backlog) {
  return append_kevent(&(struct qio_kevent){
      .op = QIO_KQ_LISTEN,

      .ke.ident = fd,
      .ke.flags = EV_ADD | EV_ONESHOT,
      .ke.filter = EVFILT_READ,

      .listen.backlog = backlog,
  });
}

QIO_API qd_t qbind(qfd_t fd, void *addr, uint64_t addrlen) {
  return append_kevent(&(struct qio_kevent){
      .op = QIO_KQ_RECV,

      .ke.ident = fd,
      .ke.flags = EV_ADD | EV_ONESHOT,
      .ke.filter = EVFILT_READ,

      .bind.addr = addr,
      .bind.addrlen = addrlen,
  });
}

#endif
#endif
