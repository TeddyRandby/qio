## QIO
QIO is a cross-platform and header-only library for performing asynchronous IO - without specifying how.
QIO leaves *you the programmer* responsible for checking on IO operations, and blocking if/when you want.
This library is meant to be an extremely simple alternative to something like `libuv`.
The interface is very small:
```c
/*
 * Abstraction over os file/pipe/console/socket types.
 * (basically, a HANDLE on Windows and an int everywhere else)
 */
typedef /* os_fd_type */ qfd_t;

/*
 * A qd (pronounced 'kid') is a handle representing a single, 'queued' IO operation.
 *
 * It is used to:
 *  - Check on the status of its corresponding operation.
 *  - Get the result of its operation
 */
typedef int64_t qd_t;

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
 * This operation is *blocking*. It blocks until the operation is complete - 
 * and then reclaims the memory of `qd` for future operations.
 * 
 *  NOTE:
 *      Currently, there is a 'free list' protected by a mutex.
 *      This allows multiple threads to queue and destroy
 *      operations in parallel.
 *
 *      This does mean there will probably be a lot of contention
 *      on this one lock. It may be possible to implement this more
 *      efficiently with a single atomic qd_t as the head of the list.
 */
void qd_destroy(qd_t qd);

/*
 * %-----------%
 * | QIO Setup |
 * %-----------%
 */

/*
 * Initialize QIO. This should only be called *once*.
 * This sets up platform-specific IO datastructures, as well as performing
 * any other initialization necessary.
 */
int32_t qio_init(uint64_t size);

/*
 * Run the event loop.
 */
int32_t qio_loop();

/*
 * De-initialize QIO. This should only be called *once*.
 *
 * In theory, this destroys the platform-specific IO datastructures setup by qio_init.
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

qd_t qread(qfd_t fd, uint64_t offset, uint64_t n, uint8_t buf[n]);
qd_t qwrite(qfd_t fd, uint64_t n, uint8_t buf[n]);

qd_t qsocket(int domain, int type, int protocol);
qd_t qaccept(qfd_t fd, void* addr, void* addrlen, uint32_t flags);
qd_t qconnect(qfd_t fd, void *addr, uint64_t addrlen);
qd_t qclose(qfd_t fd);
qd_t qshutdown(qfd_t fd, int32_t how);

qd_t qsend(qfd_t fd, uint64_t n, uint8_t buf[n]);
qd_t qrecv(qfd_t fd, uint64_t n, uint8_t buf[n]);

/* %-------------%
 * | Coming Soon |
 * %-------------%
 *
 * qlisten
 * qbind
 * qmkdir 
 * qmkdirat
 */
```
## Examples
For usage exapmles, it is best to check the `examples/` directory.
## Sockets
A quick aside - because this project aims to be cross platform, *all* sockets and addresses
are configured to be IPV6 TCP Streams. (Note the lack of domain/type/protocol arguments to `qsocket`).
This simplifies the interface and also provides (hopefully) some uniform behavior across platforms.
Do note that because all address are IPV6, localhost is written as `'::1'` instead of `'127.0.0.1'`.
### Peculiar usage notes
To simplify the interface, `qio` uses some top-level `static` variables.
This can be confusing and seem contradictory to QIO's header-only nature. And if you notice,
the typical `QIO_IMPLEMENTATION` macro guard that is often found in header-only libraries is missing.
Unlike other header-only libraries which are designed to be included multiple times throught the project
and only *defined* once (via the aforementioned macro guards), QIO is designed to only be *included* once.
The distinction here is to encourage the programmer to wrap qio's api with their own app-specific functionality.
This functionality is then compiled as one translation unit and linked where needed in the larger application.

Additionally, QIO does not try to detect the OS on its own. When building qio, it is important to define a macro
telling QIO which platform implementation to use. The options are:
- QIO_LINUX
- QIO_MACOS
- QIO_WINDOWS
If building for a single platform, it is sufficient to do the following:
```c
#define QIO_LINUX
#include "qio.h"

int main() {
    // ... application code ...
}
```
When building for multiple platforms, define the appropriate macro for each platform's build.
### LINUX
The linux implementation uses `io_uring`. IO operations are buffered into a thread-safe queue, and `qio_loop` batches requests from this queue to the kernel.
### Macos
The darwin implementation uses `kqueue`. IO operations are buffered into a thread-safe queue. `qio_loop` processes the queue. Some operations *do not require polling*, such as `open`.
These operations are performed by `qio_loop` when it comes across them. For reads/writes which do require polling, `qio_loop` batches out events to the kernel, and then tries to perform
non-blocking reads/writes for the descriptors which received events. If these operations fail with E_AGAIN or E_WOULDBLOCK, they are placed onto the queue again to try later.
#### Note:
As this implementation requires all IO operations to be non-blocking, `stdio` and the like need to be modified with `fcntl` to include `O_NONBLOCK`. This can be taken care of in `qio_init`
but isn't done yet.
### Windows
A windows implementation with IO Completion Ports is planned but not begun.
#### A note on qopen
Right now, `qopen` and `qopenat` specify `read/write` permissions manually
on all files they open. This is because I don't want to bother writing up some
cross-platform flags nonsense at the moment. For now, always opening `r/w` with `create` and `append` on is good enough.
#### Dependencies:
There are two header files included in this repo alongside `qio.h`. One is a cross-platform implementation of the c11 threads API. On linux nowadays
this is unnecessary, as `<threads.h>` likely just comes with your distribution. However other platforms are not up to speed on this optional part of the 
c-standard, and so `"threads.h"` is provided here if you need it. `"vector.h"` provides a macro-hell generic vector, with some multi-threading capabilities.
#### TODO:
The tcp echo-server example is useful for testing, but it would be much better to write a significant test-application which can really push the system.
