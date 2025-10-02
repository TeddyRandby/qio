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
typedef int32_t qd_t;

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

qd_t qwrite(qfd_t fd, uint64_t n, const uint8_t buf[n]);
qd_t qread(qfd_t fd, uint64_t offset, uint64_t n, uint8_t buf[n]);

qd_t qclose(qfd_t fd);

/*
* QIO combines the socket type\domain\protocol arguments that
* are found in posix into configurations that are common and
* cross-platform. These are TCP and UDP.
*
* Note: These use IPv6 exclusively. (For now)
*/
enum qsock_type { QSOCK_TCP, QSOCK_UDP };
qd_t qsocket(enum qsock_type type);

qd_t qconnect(qfd_t fd, const struct qio_addr *addr);
qd_t qbind(qfd_t fd, const struct qio_addr *addr);
qd_t qlisten(qfd_t fd, uint32_t backlog);
qd_t qaccept(qfd_t fd, struct qio_addr *addr_out);

qd_t qsend(qfd_t fd, uint64_t n, const uint8_t buf[n]);
qd_t qrecv(qfd_t fd, uint64_t n, uint8_t buf[n]);

qd_t qshutdown(qfd_t fd);

/*
* %------------------%
* | QIO Address TYPE |
* %------------------%
* The qio_addr struct is used to when resolving hostnames for
* qconnect, qbind, and for storing client address upon qaccept.
*
* qio_addrfrom resolves the hostname with 'getaddrinfo' on posix.
*
* This can look something like:
*   struct qio_addr info;
*   qio_addrfrom("www.google.com", 443, &info)
*   qio_addrfrom("::1", 8080, &info)
*
* NOTE: This supports *only* IPv6. This is why localhost is "::1".
*
* This function returns non-zero on error. If successful, the address information
* of the resolved hostname and port is written to dst. This is sufficient for qconnect and qbind.
*/
int qio_addrfrom(const char *restrict hostname, uint16_t port,
                         struct qio_addr *dst);
```
## Examples
For usage exapmles, it is best to check the `examples/` directory.
### Peculiar usage notes
To simplify the interface, `qio` uses some top-level `static` variables.
This can be confusing and seem contradictory to QIO's header-only nature. And if you notice,
the typical `QIO_IMPLEMENTATION` macro guard that is often found in header-only libraries is missing.
Unlike other header-only libraries which are designed to be included multiple times throught the project
and only *defined* once (via the aforementioned macro guards), QIO is designed to only be *included* once.
The distinction here is to encourage the programmer to wrap qio's api with their own app-specific functionality.
This functionality is then compiled as one translation unit and linked where needed in the larger application.

Additionally, QIO does not try to detect the OS on its own. When building qio, it is necessary to define a macro
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
As this implementation requires all IO operations to be non-blocking, `stdio` and the like need to be modified with `fcntl` to include `O_NONBLOCK`. This is taken care of in `qio_init`,
and will affect the rest of the program's behavior.
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
