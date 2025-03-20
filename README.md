## QIO
QIO is a cross-platform and header-only library for performing asynchronous IO - without specifying how.
QIO leaves *you the programmer* responsible for checking on IO operations, and blocking if/when you want.
The interface is very small:
```c
/*
 * Abstraction over os file/pipe/console/socket types.
 * (basically, a HANDLE on Windows and an int everywhere else)
 */
typedef /* os_fd_type */ qfd_t;

/*
 * A qd (pronounced 'kid') is a handle representing a single, queued IO operation.
 *
 * It is used to:
 *  - Check on the status of its corresponding operation.
 *  - Get the result of its operation
 */
typedef uint64_t qd_t;

/*
 * %----------------%
 * | QD Operations |
 * %----------------%
 */

/*
 * This function is *not* blocking. It will immediately return:
 *  - nonzero if the corresponding operation is complete.
 *  - zero if the operation is still in progress.
 */
int8_t  qd_status(qd_t qd);

/*
 * This operation is *blocking*. It blocks the caller until the qd's corresponding operation is complete,
 * and returns the return value of the queued operation.
 */
int64_t qd_result(qd_t qd);

/*
 * %-----------%
 * | QIO Setup |
 * %-----------%
 */

/*
 * Initialize QIO. This should only be called *once* per thread.
 * This sets up platform-specific IO datastructures. (Like the queues in io_uring).
 */
void qio_init(uint64_t size);

/*
 * De-initialize QIO. This should only be called *once* per thread.
 *
 * This destroys the platform-specific IO datastructures setup by qio_init.
 * 
 * Currently this just leaks all memory. Who cares? This stuff lives the whole
 * lifetime of the thread its on anyway.
 */
void qio_destroy(uint64_t size);

/* 
 * %----------%
 * | QIO API |
 * %---------%
 * The following are the 'queued' versions of corresponding POSIX functions.
 * Hopefully the interfaces are self explanatory if you're familiar with POSIX.
 */
qd_t qopen(const char* path);
qd_t qopenat(qfd_t fd, const char* path);

qd_t qread(qfd_t fd, uint64_t n, uint8_t buf[n]);
qd_t qwrite(qfd_t fd, uint64_t n, uint8_t buf[n]);

qd_t qsocket(int domain, int protocol, int type);
qd_t qaccept(qfd_t fd, void* addr, void* addrlen, uint32_t flags);

qd_t qsend(qfd_t fd, uint64_t n, uint8_t buf[n]);
qd_t qrecv(qfd_t fd, uint64_t n, uint8_t buf[n]);
```
### Peculiar usage notes
To simplify the interface, `qio` uses some top-level `static` variables.
This can be confusing and seem contradictory to QIO's header-only nature. And if you notice,
the typical `QIO_IMPLEMENTATION` macro guard that is often found in header-only libraries is missing.
Unlike other header-only libraries which are designed to be included multiple times throught the project
and only *defined* once (via the aforementioned macro guards), QIO is designed to only be *included* once.
The distinction here is to encourage the programmer to wrap qio's api with their own app-specific functionality.
This functionality is then compiled as one translation unit and linked where needed in the larger application.
### LINUX
The linux implementation uses `io_uring`. Currently, each queued command still requires a system call to `io_uring_enter` to notify
the kernel that an submission entry has been queued.
TODO:
- Batch operations which occur near each other in time. This can be done by pushing them into a local buffer, and then flushing
the whole buffer out at once via `io_uring_enter` in `qio_loop`.
- Memory usage is dumb. Operations are never *reused* or freed. (Slots in the internal queue are never reclaimed)
    - Maybe a linked-list(ish) of indexes in the vector can replace this.
### Macos
A macos implementation with kqueue is planned but not begun.
### Windows
A windows implementation with IO Completion Ports is planned but not begun.
#### A note on qopen
Right now, `qopen` and `qopenat` specify `read/write` permissions manually
on all files they open. This is because I don't want to bother writing up some
cross-platform flags nonsense at the moment. For now, always opening `r/w` is good enough.
#### TODO:
- An actual thread-safe implementation of `v_qd`. The current setup is fine for
applications queueing from only one thread, but wont scale beyond that.
