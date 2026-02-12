#include <stdio.h>

#include "threads.h"
#include "qio.h"

#define QSIZE 256

int io_loop(void *initialized) {
  if (qio_init(QSIZE) < 0)
    return 1;

  printf("[IO] Initialization complete.\n");

  *(bool *)initialized = true;

  if (qio_loop() < 0) {
    printf("[IO] Loop failed.\n");
  } else {
    printf("[IO] Ended successfully.\n");
  }

  return qio_destroy(), 0;
}

#define BUF_SIZE 1000
#define NQIDS 1000

int main() {
  _Atomic bool initialized = false;

  thrd_t t;
  if (thrd_create(&t, io_loop, &initialized) != thrd_success)
    return 1;

  while (!initialized)
    ;

  printf("[MAIN] Beginning work.\n");


  qd_t file_qid = qopen("README.md");

  printf("[MAIN] Began opening file: %i.\n", file_qid);

  qfd_t fd = qd_result(file_qid);

  struct qio_stat stat = {0};
  int64_t stat_res = qd_result(qstat(fd, &stat));

  printf("[MAIN] statres: %li\n", stat_res);

  printf("[MAIN] Opened file: %lu -> %lu\n", fd, qio_statsize(&stat));

  uint8_t buf[BUF_SIZE];
  qd_t qids[NQIDS];

  for (int i = 0; i < NQIDS; i++) {
    qids[i] = qread(fd, sizeof(buf), buf);
    printf("[QID %i] Queued read of %lu.\n", qids[i], sizeof(buf));
  }

  for (int i = 0; i < NQIDS; i++) {
    qd_t qid = qids[i];
    int64_t result = qd_result(qid);
    printf("[QID %i] Read got: %li.\n", qid, result);
  }

  thrd_join(t, nullptr);

  return 0;
}
