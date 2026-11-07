#include "queue.h"
#include "buffer.h"
#include "table.h"
#include "worker.h"
#include <asm-generic/errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/poll.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>

Sender *create_sender() {
  Sender *sender = (Sender *)malloc(sizeof(Sender));
  sender->fds = (int *)malloc(sizeof(int) * DEFAULT_SENDER_SIZE);
  sender->work_queue_arr = (Queue **)malloc(sizeof(Queue *) * DEFAULT_SENDER_SIZE);
  sender->len = 0;
  sender->size = DEFAULT_SENDER_SIZE;
  sender->completed_queue = createQueue();
  return sender;
}
void push_to_sender(Sender *sender, int fd, Work *work) {
  bool found = false;
  for (int i = 0; i < sender->len; i++) {
    if (sender->fds[i] == fd) {
      enqueue(sender->work_queue_arr[i], work);
      found = true;
      break;
    }
  }
  if (!found) {
    sender->fds[sender->len] = fd;
    Queue *q = createQueue();
    enqueue(q, work);
    sender->work_queue_arr[sender->len] = q;
    sender->len ++;
    if (sender->len >= sender->size * 0.7) {
      sender->fds = realloc(sender->fds, sizeof(int) * sender->size * 2);
      sender->work_queue_arr = realloc(sender->work_queue_arr, sizeof(Queue *) * sender->size * 2);
      sender->size *= 2;
    }
  }
}
static inline bool _endwith_EOL(const Buff *buff) {
  if (buff->len == 0) return false;
  return buff->buf[buff->len - 1] == '\n';
}
static inline int _find_fd(int fd, const int *fds, size_t len) {
  for (int i = 0; i < len; i++) {
    if (fds[i] == fd) return i;
  }
  return -1;
}
/*
  對所有傳入的 fds 內 fd 嘗試進行寫入 (from the first buffer in queue)。
  注意：我們會忽略 revents 內沒有 POLLOUT flag 的 fd。
  回傳成功寫入資料的 fds 數量。
  會將完成傳輸的 work 標記為 W_COMPLETED，
  並放入 completed works queue。
*/
int run_sender(Sender *sender, const struct pollfd *fds, size_t nfds) {
  int ret = 0;
  for (size_t i = 0; i < nfds; i++) {
    if ((fds[i].revents & POLLOUT) == 0) continue;
    int j = _find_fd(fds[i].fd, sender->fds, sender->len);
    if (j < 0) continue;
    Queue *queue = sender->work_queue_arr[j];
    Work *work = (Work *)peek(queue);
    if (work == NULL) {
      fprintf(stderr, "No work left in queue.\n");
      continue;
    }
    Buff *buff = work->buff;
    // 確認 EOL 是否存在，否則補上
    if (!_endwith_EOL(buff))
      write_buff(buff, "\n", 1);
    // 嘗試寫入 (應當為 Nonblocking)
    int n = write(fds[i].fd, buff->buf + buff->left, buff->len - buff->left);
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        continue; // Try again next round
      else {
        fprintf(stderr, "Write error in sender: %s\n", strerror(errno));
        continue;
      }
    }
    buff->left += n;
    if (buff->left >= buff->len) {
      // 傳輸完成 => dequeue、標記為完成、放入 completed queue
      dequeue(queue);
      work->status = W_COMPLETED;
      enqueue(sender->completed_queue, work);
    }
    ret ++;
  }
  return ret;
}

inline static void _free_work_void(void *work) {
  free_work((Work *)work);
}
/*
  刪除所有 queue 為空的 fd-workqueue pairs。
  回傳刪除的筆數。
*/
int flush_sender(Sender *sender) {
  int i = 0;
  int ret = 0;
  while (i != sender->len) {
    if (peek(sender->work_queue_arr[i]) != NULL) {
      i++; continue;
    }
    ret ++;
    sender->len --;
    // Remove
    freeQueue(sender->work_queue_arr[i], _free_work_void);
    // Move
    sender->fds[i] = sender->fds[sender->len];
    sender->work_queue_arr[i] = sender->work_queue_arr[sender->len];
    // Delete
    sender->fds[sender->len] = -1;
    sender->work_queue_arr[sender->len] = NULL;
  }
  return ret;
}
void free_sender(Sender *sender) {
  free(sender->fds);
  for (int i = 0; i < sender->len; i++) {
    freeQueue(sender->work_queue_arr[i], _free_work_void);
  }
  free(sender->work_queue_arr);
  free(sender);
}


Receiver *create_receiver() {
  Receiver *recver = (Receiver *)malloc(sizeof(Receiver));
  recver->fds = (int *)malloc(sizeof(int) * DEFAULT_RECVER_SIZE);
  recver->buff_queue_arr = (Queue **)malloc(sizeof(Queue *) * DEFAULT_RECVER_SIZE);
  recver->len = 0;
  recver->size = DEFAULT_RECVER_SIZE;
  return recver;
}
/*
  對每個傳入的 fds 嘗試 read 內容。
  我們會忽略 revents 缺少 POLLIN flag 的 fd。
  Read 到內容後，將 buff 對 `\n` 進行截斷，
  分別 push 到各 buff_queue 中。
  若前一個 buff_queue 中的內容沒有 `\n` 結尾，則
  將本回的 buff 接續它後面寫入。
*/
int run_receiver(Receiver *recver, const struct pollfd *fds, size_t nfds) {
  int ret = 0;
  for (int i = 0; i < nfds; i++) {
    if ((fds[i].revents & (POLLIN | POLLHUP)) == 0) continue;
    int j = _find_fd(fds[i].fd, recver->fds, recver->len);
    if (j < 0) continue;
    Queue *queue = recver->buff_queue_arr[j];
    Buff *buff = peekRear(queue);
    if (buff == NULL || _endwith_EOL(buff)) {
      buff = create_buff();
      enqueue(queue, buff);
    }
    char buf[DEFAULT_READ_SIZE];
    int n = read(fds[i].fd, buf, DEFAULT_READ_SIZE);
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        continue; // Try again next round
      else {
        fprintf(stderr, "Read failed in receiver: %s\n", strerror(errno));
        continue;
      }
    }
    // Find `\n`, split and push into queue
    int head = 0;
    for (int k = 0; k < n; k++) {
      if (buf[k] == '\n') {
        // Save content and create new buff
        write_buff(buff, buf+head, k-head+1);
        buff = create_buff();
        enqueue(queue, buff);
        head = k + 1;
      }
    }
    // 結尾非 `\n`，記得把剩餘的也寫入 buff
    if (head != n) {
      write_buff(buff, buf+head, n-head);
    }
  }
  return ret;
}
inline static void _free_buff_void(void *buff) {
  free_buff((Buff *)buff);
}
void free_receiver(Receiver *recver) {
  for (int i = 0; i < recver->len; i++) {
    freeQueue(recver->buff_queue_arr[i], _free_buff_void);
  }
  free(recver);
}

/*
  若 buff == NULL，自動建立空 Buff。
  此函式不會複製 buff 內容，將直接引用原始資料 (if not NULL)。
*/
Work *create_work(pid_t from, pid_t to, WorkType type, Buff *buff) {
  Work *work = (Work *)malloc(sizeof(Work));
  work->from = from;
  work->to = to;
  work->type = type;
  work->buff = buff;
  return work;
}
void free_work(Work *work) {
  free_buff(work->buff);
  free(work);
}

Worker *create_worker(const PidFDTable *pidfdtable) {
  Worker *worker = (Worker *)malloc(sizeof(Worker));
  worker->sender = create_sender();
  worker->recver = create_receiver();
  worker->pidfdtalbe = pidfdtable;
  return worker;
}
int push_work(Worker *worker, Work *work) {
  switch (work->type) {
    case (W_SEND): {
      int fd = search_outfd(worker->pidfdtalbe, work->to);
      if (fd < 0) return -1;
      push_to_sender(worker->sender, fd, work);
      break;
    }
    case (W_RECV): {
      fprintf(stderr, "You should not push a recv work. The receiving will start automatically for each available in_fd.\n");
      return -1;
    }
    default: {
      fprintf(stderr, "Invalid work type: %d\n", work->type);
      return -1;
    }
  }
  return 0;
}

/**
 * @brief 處理節點的工作傳輸與狀態更新。
 *
 * @details 依據傳輸策略，自動處理指定檔案描述符（FD）的讀寫操作，
 *          並將完成或狀態改變的工作記錄於輸出陣列中。
 *
 * @param[in,out] works_out  指向工作完成輸出陣列指標的指標（`Work**`）。
 *                           函式內部若發現空間不足，會自動進行 `realloc`。
 * @param[in,out] nworks_out 指向輸出陣列目前最大配額（容量）的指標。
 *                           若觸發 `realloc`，其值會被同步更新。
 * @param[in,out] consult    顧問物件指標。若為一般節點，請傳入 `NULL`。
 *
 * @return int 傳回處理完成回傳的任務數量，若出錯則傳回負值。
 *
 * @note
 * ### 傳輸策略 — Write 策略
 * 1. 每次嘗試將 `buff` 內容寫入 `pid_t to` 對應的 `out_fd`。
 * 2. 清空 `buff` 後，將該工作標記為 `@ref W_COMPLETED`。
 * 3. 將完成的工作放回 @p works_out 陣列中。
 *
 * @note
 * ### 傳輸策略 — Read 策略
 * 1. 自動對所有 `in_fds` 建立 `RECV` 任務。
 * 2. 嘗試讀取內容至 `buff`，直到遇見換行符 `\n`。
 * 3. 遇見換行符後，將該任務標記為 `@ref W_COMPLETED` 並放入 @p works_out，於下一輪建立全新的 `RECV` 任務。
 * 4. 若讀取到 `EOF`，則將狀態設置為 `@ref W_EOF` 後放入 @p works_out。
 */
int run_worker(
  Worker *worker,
  Work ***works_out,
  size_t *works_out_n,
  Consultant *consult
) {
  // 整理需要 poll 的 fds
  struct pollfd *fds = (struct pollfd *)malloc(sizeof(struct pollfd) * MAX_FD_PID_PAIRS * 2);
  size_t nfds = 0;
  size_t size = MAX_FD_PID_PAIRS * 2;
  for (size_t i = 0; i < worker->recver->len;) {
    if (search_pid_in(worker->pidfdtalbe, worker->recver->fds[i]) != NULL_PID) {
      i++;
      continue;
    }
    freeQueue(worker->recver->buff_queue_arr[i], _free_buff_void);
    worker->recver->len--;
    worker->recver->fds[i] = worker->recver->fds[worker->recver->len];
    worker->recver->buff_queue_arr[i] = worker->recver->buff_queue_arr[worker->recver->len];
  }
  // sender 內有殘餘工作的 fd
  for (int i = 0; i < worker->sender->len; i++) {
    int fd = worker->sender->fds[i];
    // Check fd 是否合法 (可能已死亡)
    if (search_pid_in(worker->pidfdtalbe, fd) == NULL_PID) {
      if (consult == NULL || (consult != NULL && consult->in_fd == fd)) {
        // Dead fd => remove from sender
        // 將 Work Queue 清空，之後統一 flush 掉
        Queue *q = worker->sender->work_queue_arr[i];
        Work *work;
        while ((work = dequeue(q)) != NULL) {
          free_work(work);
        }
        continue;
      }
    }
    fds[nfds].fd = fd;
    fds[nfds].events = POLLOUT;
    nfds++;
    if (nfds >= size * 0.7) {
      fds = (struct pollfd *)realloc(fds, sizeof(struct pollfd) * size * 2);
      size *= 2;
    }
  }
  // 所有的 in_fd (包含 Judge, Search, 及所有 child)
  for (int i = 0; i < worker->pidfdtalbe->len; i++) {
    int in_fd = worker->pidfdtalbe->in_fds[i];
    if (in_fd < 0) continue;
    if (_find_fd(in_fd, worker->recver->fds, worker->recver->len) < 0) {
      if (worker->recver->len == worker->recver->size) {
        size_t new_size = worker->recver->size * 2;
        worker->recver->fds = realloc(worker->recver->fds, sizeof(int) * new_size);
        worker->recver->buff_queue_arr = realloc(worker->recver->buff_queue_arr, sizeof(Queue *) * new_size);
        worker->recver->size = new_size;
      }
      size_t idx = worker->recver->len++;
      worker->recver->fds[idx] = in_fd;
      worker->recver->buff_queue_arr[idx] = createQueue();
    }
    fds[nfds].fd = in_fd;
    fds[nfds].events = POLLIN;
    nfds++;
    if (nfds >= size * 0.7) {
      fds = (struct pollfd *)realloc(fds, sizeof(struct pollfd) * size * 2);
      size *= 2;
    }
  }
  // Consult in if not NULL
  if (consult != NULL) {
    fds[nfds].fd = consult->in_fd;
    fds[nfds].events = POLLIN;
    nfds++;
  }

  // poll
  if (poll(fds, nfds, -1) < 0) {
    fprintf(stderr, "Poll error: %s\n", strerror(errno));
    free(fds);
    return -1;
  }

  // 跑一遍 sender & receiver
  run_sender(worker->sender, fds, nfds);
  run_receiver(worker->recver, fds, nfds);

  // 取出 completed works 並放入 works_out
  int ret = 0;
  // sender
  Queue *q = worker->sender->completed_queue;
  Work *work;
  while ((work = peek(q)) != NULL) {
    dequeue(q);
    (*works_out)[ret] = work;
    ret ++;
    if (ret >= *works_out_n * 0.7) {
      *works_out = (Work **)realloc(*works_out, sizeof(Work *) * (*works_out_n) * 2);
      *works_out_n *= 2;
    }
  }
  flush_sender(worker->sender);
  // receiver
  for (int i = 0; i < worker->recver->len; i++) {
    Queue *q = worker->recver->buff_queue_arr[i];
    int fd = worker->recver->fds[i];
    pid_t pid_from = search_pid_inout(worker->pidfdtalbe, fd);
    if (pid_from == NULL_PID) {
      fprintf(stderr, "FD not found in pidfdtable: %d\n", fd);
      continue;
    }
    Buff *buff;
    while ((buff = peek(q)) != NULL && (buff->len == 0 || _endwith_EOL(buff))) {
      Work *work = create_work(
        pid_from, NULL_PID, W_RECV, buff);
      if (buff->len == 0) {
        work->status = W_EOF;
      } else {
        work->status = W_COMPLETED;
      }
      dequeue(q);
      (*works_out)[ret] = work;
      ret++;
      if (ret >= *works_out_n * 0.7) {
        *works_out = (Work **)realloc(*works_out, sizeof(Work *) * (*works_out_n) * 2);
        *works_out_n *= 2;
      }
    }
  }

  free(fds);
  return ret;
}
void free_worker(Worker *worker) {
  free_sender(worker->sender);
  free_receiver(worker->recver);
  free(worker);
}

