#ifndef QIO_H
#define QIO_H

#include <stdint.h>

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

#include <unistd.h>
/* Implemented using kqueue */

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

QIO_API qd_t qread(qfd_t fd, int64_t off, uint64_t n, uint8_t buf[n]);
QIO_API qd_t qwrite(qfd_t fd, uint64_t n, uint8_t buf[n]);

QIO_API qd_t qsocket(int domain, int protocol, int type);
QIO_API qd_t qaccept(qfd_t fd, void *addr, void *addrlen, uint32_t flags);
QIO_API qd_t qconnect(qfd_t fd, void *addr, uint64_t addrlen);
QIO_API qd_t qclose(qfd_t fd);

QIO_API qd_t qsend(qfd_t fd, uint64_t n, uint8_t buf[n]);
QIO_API qd_t qrecv(qfd_t fd, uint64_t n, uint8_t buf[n]);

struct qio_op_t {
  int8_t done;
  int32_t result;
  uint32_t flags;
};

#define T struct qio_op_t
#define NAME qd
#define V_CONCURRENT
#include "vector.h"

/*
 * The following variable (and other platform-specific globals like it)
 * are marked static, and not _Thread_local. This is intentional -
 * it is almost *always* the case that IO operations qre queued from
 * a different thread than the one running the qio_loop. Because of this,
 * these data structures need to be thread-safe and static.
 * TODO: FIXME: Make this data-structure thread-safe.
 */
static v_qd qds;

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
  while (true) {
    int buffered_sqes = queued_sqes.len;
    assert(buffered_sqes < ring_entries);
    /*
     * Issues, as sqe's can be added *during* this loop.
     */
    for (int i = 0; i < buffered_sqes; i++) {
      struct io_uring_sqe src_sqe = v_sqe_pop(&queued_sqes);

      unsigned index, tail;
      tail = *sring_tail;
      index = tail & *sring_mask;

      assert(src_sqe.user_data < qds.len);
      struct io_uring_sqe *dst_sqe = &sqes[index];
      memcpy(dst_sqe, &src_sqe, sizeof(struct io_uring_sqe));

      sring_array[index] = index;
      tail++;

      /* Update the tail */
      io_uring_smp_store_release(sring_tail, tail);
    }

    /* System call to trigger kernel */
    if (buffered_sqes)
      io_uring_enter(ring, buffered_sqes, 0, 0, nullptr);

    struct io_uring_cqe *cqe;
    unsigned head;

    /* Read barrier */
    head = io_uring_smp_load_acquire(cring_head);

    /* If head == tail, buffer is empty */
    if (head == *cring_tail)
      continue;

    /* Get the entry */
    unsigned index = head & (*cring_mask);
    cqe = &cqes[index];
    head++;

    qd_t qid = cqe->user_data;

    assert(qid < qds.len);
    assert(v_qd_val_at(&qds, qid).done == false);
    assert(v_qd_val_at(&qds, qid).flags == 0);
    assert(v_qd_val_at(&qds, qid).result == 0);
    /*
     * Perform this via a set for two resons:
     *  - The struct is small enough that passying and copying by value is fine
     *  - This performes the update in *one atomic operation*. Holding pointers
     *    into the vector is *unsafe* in a concurrent vector, as the array could
     *    be reallocated out from underneath you.
     */
    v_qd_set(&qds, qid,
             (struct qio_op_t){
                 .result = cqe->res,
                 .flags = cqe->flags,
                 .done = true,
             });
    assert(v_qd_val_at(&qds, qid).done == true);
    assert(v_qd_val_at(&qds, qid).flags == cqe->flags);
    assert(v_qd_val_at(&qds, qid).result == cqe->res);

    /* Write barrier so that update to the head are made visible */
    io_uring_smp_store_release(cring_head, head);
  }
}

QIO_API void qio_destroy() {}

QIO_API int32_t qio_init(uint64_t size) {
  /* Initialize qds. All OS's need to do this. */
  ring_entries = size;
  v_qd_create(&qds, 64);

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
  qd_t qid = v_qd_push(&qds, (struct qio_op_t){});
  assert(qds.len > 0);

  src_sqe->user_data = qid;
  v_sqe_push(&queued_sqes, *src_sqe);

  return qid;
}

qd_t qopen(const char *path) { return qopenat(AT_FDCWD, path); }

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

QIO_API qd_t qread(qfd_t fd, int64_t offset, uint64_t n, uint8_t buf[n]) {
  assert(n < UINT32_MAX);
  return append_sqe(&(struct io_uring_sqe){
      .opcode = IORING_OP_READ,
      .fd = fd,
      .addr = (uintptr_t)buf,
      .len = n,
      .off = offset,
  });
}

qd_t qwrite(qfd_t fd, uint64_t n, uint8_t buf[n]) {
  assert(n < UINT32_MAX);
  return append_sqe(&(struct io_uring_sqe){
      .opcode = IORING_OP_WRITE,
      .fd = fd,
      .addr = (uintptr_t)buf,
      .len = n,
  });
}

qd_t qsend(qfd_t fd, uint64_t n, uint8_t buf[n]) {
  assert(n < UINT32_MAX);
  return append_sqe(&(struct io_uring_sqe){
      .opcode = IORING_OP_SEND,
      .fd = fd,
      .addr = (uintptr_t)buf,
      .len = n,
  });
}

qd_t qrecv(qfd_t fd, uint64_t n, uint8_t buf[n]) {
  return append_sqe(&(struct io_uring_sqe){
      .opcode = IORING_OP_RECV,
      .fd = fd,
      .addr = (uintptr_t)buf,
      .len = n,
  });
}

qd_t qsocket(int domain, int type, int protocol) {
  return append_sqe(&(struct io_uring_sqe){
      .opcode = IORING_OP_SOCKET,
      .fd = domain,
      .len = protocol,
      .off = type,
  });
}

qd_t qclose(qfd_t fd) {
  return append_sqe(&(struct io_uring_sqe){
      .opcode = IORING_OP_CLOSE,
      .fd = fd,
  });
}

qd_t qaccept(qfd_t fd, void *addr, void *addrlen, uint32_t flags) {
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

#endif
#endif
