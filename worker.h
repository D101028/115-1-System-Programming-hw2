#ifndef WORKER_H
#define WORKER_H

#include "table.h"
#include "buffer.h"
#include "queue.h"
#include <stdio.h>
#include <sys/types.h>
#include <poll.h>

#define MAX_FD_PID_PAIRS 32
#define DEFAULT_SENDER_SIZE 32
#define DEFAULT_RECVER_SIZE 32
#define DEFAULT_READ_SIZE 1024

typedef enum { W_SEND, W_RECV } WorkType;
typedef enum { W_INCOMPLETED, W_COMPLETED, W_EOF } WorkStatus;
// 當 W_COMPLETED 被標記後，需在後續處理中手動調用 `free_work` 清理記憶體。
typedef struct {
  pid_t from;       // 若 type 為 W_SEND，某種程度我們同意將 from 設置為 NULL_PID 代表自身的 pid。
  pid_t to;         // 若 type 為 W_RECV，某種程度我們同意將 to 設置為 NULL_PID 代表自身的 pid。
  WorkType type;
  WorkStatus status;
  Buff *buff;
} Work;

typedef struct {
  int *fds;
  Queue **work_queue_arr;
  size_t len;
  size_t size;
  Queue *completed_queue;  // queue to save completed works
} Sender;
Sender *create_sender();
void push_to_sender(Sender *sender, int fd, Work *work);
int run_sender(Sender *sender, const struct pollfd *fds, size_t nfds);
int flush_sender(Sender *sender);
void free_sender(Sender *sender);

typedef struct {
  int *fds;
  Queue **buff_queue_arr;
  size_t len;
  size_t size;
} Receiver;
Receiver *create_receiver();
int run_receiver(Receiver *recver, const struct pollfd *fds, size_t nfds);
void free_receiver(Receiver *recver);

Work *create_work(pid_t from, pid_t to, WorkType type, Buff *buff);
void free_work(Work *work);

typedef struct {
  Sender *sender;
  Receiver *recver;
  const PidFDTable *pidfdtalbe;
} Worker;

typedef struct {
  pid_t pid;    // Fake PID by default
  int in_fd;    // Consultant read
  int out_fd;   // Consultant write
  Buff *path;
} Consultant;

Worker *create_worker(const PidFDTable *pidfdtable);
int push_work(Worker *worker, Work *work);
int run_worker(
  Worker *worker, 
  Work ***works_out, 
  size_t *works_out_n, 
  Consultant *consult
);
void free_worker(Worker *worker);

#endif