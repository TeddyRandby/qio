#include "qio.h"
#include <stdio.h>

#define QSIZE 256

int io_loop(void *initialized) {
  if (qio_init(QSIZE) < 0)
    return 1;

  printf("[IO] Initialization complete.\n");

  *(bool *)initialized = true;

  if (qio_loop() < 0)
    return qio_destroy(), 1;

  return qio_destroy(), 0;
}

#define BUF_SIZE 1000
#define NQIDS 1000

int main() {
  bool initialized = false;

  thrd_t t;
  if (thrd_create(&t, io_loop, &initialized) != thrd_success)
    return 1;

  while (!initialized)
    ;

  qd_t file_qid = qopen("README.md");

  int fd = qd_result(file_qid);

  uint8_t buf[BUF_SIZE];
  qd_t qids[NQIDS];

  for (int i = 0; i < NQIDS; i++) {
    qids[i] = qread(fd, 0, sizeof(buf), buf);
    printf("[QID %li] Queued read of %lu.\n", qids[i], sizeof(buf));
  }

  for (int i = 0; i < NQIDS; i++) {
    qd_t qid = qids[i];
    int64_t result = qd_result(qid);
    printf("[QID %li] Read got: %li.\n", qid, result);
  }

  thrd_join(t, nullptr);

  return 0;
}
