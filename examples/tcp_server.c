#include "../qio.h"

#include <stdio.h>
#include <string.h>

#define QSIZE 256

const int server_port = 8077;

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

/*
 * A typical IO loop for qio. Uses a bool* to notify parent thread when qio
 * initialization is complete.
 */
int io_loop(void *initialized) {
  if (qio_init(QSIZE) < 0)
    return 1;

  *(bool *)initialized = true;

  if (qio_loop() < 0)
    return qio_destroy(), 1;

  return qio_destroy(), 0;
}

int io_client(void *) {
  int client_sock = qd_destroy(qsocket(QSOCK_TCP));

  if (client_sock < 0)
    return printf("[ERROR] Failed to create server socket: %s.\n",
                  strerror(-client_sock)),
           1;

  struct qio_addr addr;

  if (qio_addrfrom("::1", server_port, &addr) < 0)
    return printf("[ERROR] Failed to create address\n"), 1;

  int res = qd_destroy(qconnect(client_sock, &addr));

  if (res < 0)
    return printf("[ERROR] Failed to connect: %s\n", strerror(-res)), 1;

  const char *msg = "MAGIC_MESSAGE";
  const size_t n = strlen(msg);
  const char buf[n];

  res = qd_destroy(qsend(client_sock, n, (uint8_t *)msg));

  if (res < 0)
    return printf("[ERROR] Failed to send: %s\n", strerror(-res)), 1;

  res = qd_destroy(qrecv(client_sock, n, (uint8_t *)buf));

  if (res < 0)
    return printf("[ERROR] Failed to recv: %s\n", strerror(-res)), 1;

  for (size_t i = 0; i < n; i++) {
    // Double check that our echo server echoes
    if (buf[i] != msg[i])
      return printf("[ERROR] Echo server sent mismstched byte %c, but expected "
                    "%c\n",
                    buf[i], msg[i]),
             1;
  }

  res = qd_destroy(qshutdown(client_sock));

  if (res < 0)
    return printf("[ERROR] Failed to close: %s\n", strerror(-res)), 1;

  res = qd_destroy(qclose(client_sock));

  if (res < 0)
    return printf("[ERROR] Failed to close: %s\n", strerror(-res)), 1;

  return 0;
}

#define N_CLIENTS 10

int main() {
  bool initialized = false;

  thrd_t io_t;
  if (thrd_create(&io_t, io_loop, &initialized) != thrd_success)
    return 1;

  while (!initialized)
    ;

  int server_sock = qd_result(qsocket(QSOCK_TCP));

  if (server_sock < 0)
    return printf("[ERROR] Failed to create server socket: %s.\n",
                  strerror(-server_sock)),
           1;

  struct qio_addr addr;

  if (qio_addrfrom("localhost", server_port, &addr) < 0)
    return printf("[ERROR] Failed to create address\n"), 1;

  qd_t bqd = qbind(server_sock, &addr);

  int res = qd_result(bqd);

  if (res < 0)
    return printf("[ERROR] Could not bind socket: %s\n", strerror(-res)), 1;

  qd_destroy(bqd);

  const int wait_size = 16;
  qd_t lqd = qlisten(server_sock, wait_size);
  if (qd_result(lqd) < 0)
    return printf("[ERROR] Could not open socket for listening\n"), 1;

  qd_destroy(lqd);

  printf("[INFO] Listening on port %i\n", server_port);

  for (int i = 0; i < N_CLIENTS; i++) {
    thrd_t cl;
    if (thrd_create(&cl, io_client, nullptr) != thrd_success)
      return printf("[ERROR] Failed to spawn client"), 1;
  }

  // socket address used to store client address
  struct sockaddr_in client_address;
  int client_address_len = 0;

  const int MAX_CONCURRENT_CLIENTS = 16;
  const int MAX_MESSAGE_BYTES = 256;

  qfd_t clients[MAX_CONCURRENT_CLIENTS];
  qd_t accepts[MAX_CONCURRENT_CLIENTS];
  qd_t recvs[MAX_CONCURRENT_CLIENTS];
  qd_t sends[MAX_CONCURRENT_CLIENTS];
  char buffer[MAX_CONCURRENT_CLIENTS][MAX_MESSAGE_BYTES];

  // Initialize our various buffers.
  for (int i = 0; i < MAX_CONCURRENT_CLIENTS; i++) {
    accepts[i] = -1;
    clients[i] = -1;
    recvs[i] = -1;
    sends[i] = -1;
  }

  while (true) {
    /*
     * The logic here is to loop through all our possible clients in a sort of
     * state machine. No iteration of this loop will block - all of the IO
     * functions are *queued*.
     *
     * The status is checked before calling qd_result on any given operation, so
     * we can be sure these calls won't block as well.
     */
    for (int i = 0; i < MAX_CONCURRENT_CLIENTS; i++) {
      // This client slot has no connected client. Queue an accept.
      if (clients[i] == -1 && accepts[i] == -1) {
        accepts[i] = qaccept(server_sock, &addr);
        printf("[INFO]: Queued accept for %i: %li\n", i, accepts[i]);
        continue;
      }

      qd_t queued_accept = accepts[i];

      if (queued_accept != -1) {
        /*printf("[INFO]: Checking queued accept %li\n", queued_accept);*/
        if (qd_status(queued_accept)) {
          qfd_t client = qd_result(queued_accept);

          if (client < 0)
            return printf("[ERROR]: Could not open a socket to accept data\n"),
                   1;

          clients[i] = client;
          assert(recvs[i] == -1);
          assert(sends[i] == -1);

          printf("[INFO]: Client %i connected with ip address: %s\n", i,
                 inet_ntoa(client_address.sin_addr));

          // Clear the accept qid.
          qd_destroy(queued_accept);
          accepts[i] = -1;
          continue;
        }
      }

      qfd_t client = clients[i];
      /*printf("[INFO]: Processing client %i\n", i);*/

      // No client is connected.
      if (client == -1) {
        /*printf("[INFO]: No client at connection %i\n", i);*/
        continue;
      }

      // No recv is queued for this client.
      if (recvs[i] == -1) {
        recvs[i] = qrecv(client, MAX_MESSAGE_BYTES, (uint8_t *)buffer[i]);
        printf("[INFO]: Queued recv for client %i: %li\n", i, recvs[i]);
        continue;
      }

      qd_t queued_recv = recvs[i];

      // A recv is ready for this client
      if (queued_recv != -1) {
        /*printf("[INFO]: Checking queued recv %li\n", queued_recv);*/
        if (qd_status(queued_recv)) {
          int64_t n = qd_result(queued_recv);
          printf("[INFO %i]: Queued recv %li is done: %li.\n", i, queued_recv,
                 n);

          // Client has requested a shutdown.
          if (n == 0) {
            printf("[INFO]: Shutdown requested for %i.\n", i);
            // Synchronous close is fine here.
            qd_destroy(qclose(client));
            qd_destroy(queued_recv);

            clients[i] = -1;
            recvs[i] = -1;

            continue;
          }

          if (n < 0)
            return printf("[ERROR] Client receive failed\n"), 1;

          // If we aren't already sending, queue a send of our recv.
          if (sends[i] == -1) {
            sends[i] = qsend(client, n, (uint8_t *)buffer[i]);
            printf("[INFO]: Queued send for client %i: %li\n", i, sends[i]);
            continue;
          }
        }
      }

      qd_t queued_send = sends[i];

      // A send is done for this client
      if (queued_send != -1) {
        /*printf("[INFO]: Checking queued send %li\n", queued_recv);*/
        if (qd_status(queued_send)) {
          int64_t n = qd_result(queued_send);
          printf("[INFO]: Queued send %li is done: %li.\n", queued_send, n);

          if (n < 0)
            return printf("[ERROR] Client send failed\n"), 1;

          printf("[INFO]: Sent %li bytes to client %i\n", n, i);

          // This client is ready for a new recv/send cycle.
          qd_destroy(sends[i]);
          qd_destroy(recvs[i]);
          sends[i] = -1;
          recvs[i] = -1;
          continue;
        }
      }
    }
  }

  close(server_sock);

  thrd_join(io_t, nullptr);

  return 0;
}
