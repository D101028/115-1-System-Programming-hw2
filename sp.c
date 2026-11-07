#include "table.h"
#include "buffer.h"
#include "worker.h"
#include "sp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>

args_t argparser(int argc, char *argv[]) {
  args_t args;
  memset(&args, 0, sizeof(args_t));
  if (argc == 1) {
    args.is_main_sp = true;
    return args;
  } else {
    args.is_main_sp = false;
  }
  if (argc != 6) {
    fprintf(stderr, "argparser error: invalid argument numbers\n");
    exit(1);
  }
  args.cmd_path = argv[1];
  args.rply_path = argv[2];
  args.search_in_fd = atoi(argv[3]);
  args.search_out_fd = atoi(argv[4]);
  args.judge_pid = atoi(argv[5]);
  return args;
}

int set_nonblocking(int fd) {
  int flags;

  if ((flags = fcntl(fd, F_GETFL, 0)) == -1) {
    perror("fcntl F_GETFL");
    return -1;
  }

  flags |= O_NONBLOCK;

  if (fcntl(fd, F_SETFL, flags) == -1) {
    perror("fcntl F_SETFL");
    return -1;
  }

  return 0;
}

// 建立好 cmd & rply FIFOs 並設置好 close on exec & nonblocking flag、導向 stdin/stdout
int create_fifos_at_stdio(char *cmd_path, char *rply_path) {
  if (cmd_path == NULL || rply_path == NULL) {
    fprintf(stderr, "Invalid fifo path\n");
    return -1;
  }

  if (mkfifo(cmd_path, 0666) < 0 && errno != EEXIST) {
    fprintf(stderr, "建立 cmd_fifo 失敗: %s\n", strerror(errno));
    return -1;
  }
  if (mkfifo(rply_path, 0666) < 0 && errno != EEXIST) {
    fprintf(stderr, "建立 rply_fifo 失敗: %s\n", strerror(errno));
    return -1;
  }

  int fifo_in = open(cmd_path, O_RDONLY | O_CLOEXEC);
  if (fifo_in < 0) {
    fprintf(stderr, "開啟 cmd_fifo 失敗: %s\n", strerror(errno));
    return -1;
  }
  int fifo_out = open(rply_path, O_WRONLY | O_CLOEXEC);
  if (fifo_out < 0) {
    fprintf(stderr, "開啟 rply_fifo 失敗: %s\n", strerror(errno));
    close(fifo_in);
    return -1;
  }

  if (dup2(fifo_in, STDIN_FILENO) < 0) {
    fprintf(stderr, "Duplicate fifo_in 失敗: %s\n", strerror(errno));
    close(fifo_in); close(fifo_out);
    return -1;
  }
  close(fifo_in);
  if (dup2(fifo_out, STDOUT_FILENO) < 0){
    fprintf(stderr, "Duplicate fifo_out 失敗: %s\n", strerror(errno));
    close(fifo_in); close(fifo_out);
    return -1;
  }
  close(fifo_out);
  unlink(cmd_path);
  unlink(rply_path);

  return 0;
}

// 建立 consultant，回傳 Consultant
// fds[0]: read fd, fds[1]: write fd
Consultant *create_consultant(int fds[2]) {
  // 建立 intra process pipe
  // fds1[0]: consultant read, fds1[1]: process write
  // fds2[0]: process read, fds2[1]: consultant write
  int fds1[2], fds2[2];
  if (pipe(fds1) < 0) {
    fprintf(stderr, "Pipe error: %s\n", strerror(errno));
    return NULL;
  }
  if (pipe(fds2) < 0) {
    fprintf(stderr, "Pipe error: %s\n", strerror(errno));
    close(fds1[0]); close(fds1[1]);
    return NULL;
  }
  // Set Nonblocking
  set_nonblocking(fds1[0]);
  set_nonblocking(fds1[1]);
  set_nonblocking(fds2[0]);
  set_nonblocking(fds2[1]);

  fds[0] = fds2[0];
  fds[1] = fds1[1];
  Consultant *consult = (Consultant *)malloc(sizeof(Consultant));
  consult->pid = FAKE_PID;
  consult->in_fd = fds1[0];
  consult->out_fd = fds2[1];
  consult->path = NULL;
  return consult;
}

void update_consultant(Consultant *consult, const Buff *new_path) {
  Buff *buff = create_buff();
  copy_buff(buff, new_path);
  free_buff(consult->path);
  consult->path = buff;
}

static int write_all(int fd, const char *buf, size_t len) {
  size_t off = 0;
  while (off < len) {
    ssize_t n = write(fd, buf + off, len - off);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return -1;
    off += (size_t)n;
  }
  return 0;
}

/* Run consultants until one returns a complete Ok line. */
static Buff *consultant_lookup(Consultant *consult, const Buff *key) {
  for (;;) {
    int to_child[2], from_child[2];
    if (pipe(to_child) < 0) return NULL;
    if (pipe(from_child) < 0) {
      close(to_child[0]); close(to_child[1]);
      return NULL;
    }
    pid_t pid = fork();
    if (pid == 0) {
      dup2(to_child[0], STDIN_FILENO);
      dup2(from_child[1], STDOUT_FILENO);
      close(to_child[0]); close(to_child[1]);
      close(from_child[0]); close(from_child[1]);
      if (consult->path != NULL) {
        consult->path->buf[consult->path->len] = '\0';
        execl(consult->path->buf, consult->path->buf, (char *)NULL);
      }
      _exit(127);
    }
    close(to_child[0]); close(from_child[1]);
    if (pid < 0) { close(to_child[1]); close(from_child[0]); return NULL; }
    int sent = write_all(to_child[1], key->buf, key->len) == 0 &&
               write_all(to_child[1], "\n", 1) == 0;
    close(to_child[1]);
    Buff *line = create_buff();
    char c;
    bool complete = false;
    while (read(from_child[0], &c, 1) == 1) {
      if (c == '\n') { complete = true; break; }
      write_buff(line, &c, 1);
    }
    close(from_child[0]);
    int status;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (!sent || !complete || line->len < 4 ||
        memcmp(line->buf, "Ok: ", 4) != 0) {
      free_buff(line);
      continue;
    }
    Buff *value = create_buff();
    write_buff(value, line->buf + 4, line->len - 4);
    free_buff(line);
    return value;
  }
}

// 處理 consultant 收到的工作
void run_consultant(Controller *contr, Work *work) {
  if (work->type != W_RECV) {
    fprintf(stderr, "run_consultant 只處理 W_RECV 任務 (Consultant 送出資料的時間一定是 fork 後，以 STDOUT 的方式給 Main SP)\n");
    return;
  }
  if (work->status == W_EOF) return; // Main SP 在處理關機程序，直接返回即可
  // 參數解析
  Command cmd;
  parse_command(work, &cmd);
  if (cmd.type != FETCH) {
    fprintf(stderr, "Consultant 不支援此類別的指令: %d\n", cmd.type);
    return ;
  }
  Buff **out = (Buff **)malloc(sizeof(Buff *) * 3);
  size_t n_out = 3;
  int n = split(work->buff, ' ', &out, &n_out);
  if (n != 3) {
    fprintf(stderr, "Invalid fetch command length: %d\n", n);
    for (int i = 0; i < n; i++) {
      free_buff(out[i]);
    }
    free(out);
    return ;
  }

  // 啟動 Consultant Program
  pid_t pid;
  Consultant *consult = contr->consult;
  if ((pid = fork()) == 0) {
    // 關閉不必要之 fds
    close_fds_all(&contr->pid_fd_table);
    // 將自身的 in, out fd 轉向 STDIO
    dup2(consult->in_fd, STDIN_FILENO);
    dup2(consult->out_fd, STDOUT_FILENO);
    close(consult->in_fd); close(consult->out_fd);
    // 執行 Program
    consult->path->buf[consult->path->len] = '\0'; // 補結尾
    execl(consult->path->buf, consult->path->buf, NULL);
  }
  // 關閉 consult in out fd
  close(consult->in_fd); close(consult->out_fd);
  // 更新當前 search_pid & fd pid table
  int in_fd, out_fd;
  in_fd = search_infd(&contr->pid_fd_table, contr->search_pid);
  out_fd = search_outfd(&contr->pid_fd_table, contr->search_pid);
  del_pidfdtable(&contr->pid_fd_table, contr->search_pid);
  contr->search_pid = pid;
  add_pidfdtable(&contr->pid_fd_table, pid, in_fd, out_fd);
  // 製造寫入 key 的 work
  Buff *key = create_buff();
  copy_buff(key, out[2]);
  write_buff(key, "\n", 1);
  work = create_work(contr->pid, pid, W_SEND, key);
  push_work(contr->worker, work);

  // Free
  for (int i = 0; i < n; i++) {
    free_buff(out[i]);
  }
  free(out);
}

void free_consultant(Consultant *consult) {
  free_buff(consult->path);
  free(consult);
}

void init_controller(Controller *contr, args_t args) {
  contr->is_main_sp = args.is_main_sp;
  // 填入自身 pid
  contr->pid = getpid();

  // 創建 KeyValueTable
  contr->kv_table = create_key_value_table();

  // 初始化 PidFDTable
  init_pidfdtable(&contr->pid_fd_table);

  // 取得 judge_pid
  if (args.is_main_sp) {
    contr->judge_pid = getppid();
  } else {
    contr->judge_pid = args.judge_pid;
  }
  // Create FIFOs to Judge and redirect to stdio
  // Main SP 直接使用 STDIO
  if (!args.is_main_sp) {
    // Normal Node
    create_fifos_at_stdio(args.cmd_path, args.rply_path);
  }
  // 將 judge pid & fds 資料填入表格
  add_pidfdtable(
    &contr->pid_fd_table,
    contr->judge_pid,
    STDIN_FILENO, STDOUT_FILENO
  );

  // 建立路由表
  contr->route_table = create_route_table();

  // 取得向上的 search pid
  // Main SP: 創建 consultant manager
  // Normal Node: 使用 parent fd
  pid_t search_pid;
  int search_in_fd, search_out_fd;
  contr->consult = NULL;
  if (args.is_main_sp) {
    int fds[2];
    Consultant *consult = create_consultant(fds);
    contr->consult = consult;
    const char *path = getenv("JUDGE_CONSULTANT");
    if (path != NULL) {
      Buff initial = {(char *)path, strlen(path), strlen(path) + 1, 0};
      update_consultant(consult, &initial);
    }
    search_pid = FAKE_PID;
    search_in_fd = fds[0]; search_out_fd = fds[1];
  } else {
    search_pid = getppid();
    search_in_fd = args.search_in_fd;
    search_out_fd = args.search_out_fd;
  }
  contr->search_pid = search_pid;
  // 更新表格
  add_pidfdtable(
    &contr->pid_fd_table,
    contr->search_pid,
    search_in_fd, search_out_fd
  );

  // Create Worker
  contr->worker = create_worker(&contr->pid_fd_table);
}

int run_controller(Controller *contr) {
  // int curr_size = DEFAULT_WORKS_NUM;
  size_t nworks = DEFAULT_WORKS_NUM;
  Work **works_out = (Work **)malloc(sizeof(Work *) * nworks);
  bool to_shutdown = false; // 終止信號
  int shutdown_loops = 0;
  while (1) {
    memset(works_out, 0, sizeof(Work *) * nworks);
    int ret = run_worker(
      contr->worker,
      &works_out,
      &nworks,
      (contr->is_main_sp ? contr->consult : NULL)
    );
    if (ret < 0) {
      fprintf(stderr, "任務執行失敗\n");
      break;
    }
    // 處理任務
    for (int i = 0; i < nworks; i++) {
      Work *work = works_out[i];
      // Main SP 代理 consultant
      // (在 Main SP 中，consultant 在收到指令前，
      //  會是由 Main SP 同步監聽的)
      if (contr->is_main_sp && work->to == contr->search_pid) {
        run_consultant(contr, work);
      }
      // RECV & W_COMPLETED
      else if (work->type == W_RECV && work->status == W_COMPLETED) {
        // 查詢結果回傳
        if (work->from == contr->search_pid) {
          // Parse Hash
          Buff **out = (Buff **)malloc(sizeof(Buff *) * 2);
          size_t n_out = 2;
          int n_buff = split(work->buff, ' ', &out, &n_out);
          if (n_buff != 2) {
            fprintf(stderr, "Invalid reply length: %d\n", n_buff);
          } else {
            // Check Route Table
            pid_t rply_pid;
            if ((rply_pid = search_pid_by_hash(contr->route_table, out[0])) == NULL_PID) {
              // 查無 pid
              out[0]->buf[out[0]->len] = '\0';
              fprintf(stderr, "RouteTable 查無 Hash: %s\n", out[0]->buf);
            } else {
              // 建立傳輸工作
              Work *new_work = create_work(
                contr->pid, rply_pid, W_SEND, work->buff);
              work->buff = NULL; // 防止傳送資料被 free
              push_work(contr->worker, new_work);
              // 把 hash 從路由表中清除
              del_route_table_by_hash(contr->route_table, out[0]);
            }
          }
          // Free
          for (int i = 0; i < n_buff; i++) {
            free_buff(out[i]);
          }
          free(out);
        }
        // 給自己的指令
        else if (work->to == contr->pid || work->to == NULL_PID) {
          Command cmd;
          parse_command(work, &cmd);
          int ret = command_handler(contr, &cmd);
          if (ret > 0) {
            // Kill cmd from Judge
            to_shutdown = true;
          }
        }
        else {
          fprintf(stderr, "Unknown work.\n");
        }
      }
      // SEND (do nothing)
      else if (work->type == W_SEND && work->status == W_COMPLETED) {}
      // RECV & W_EOF
      else if (work->type == W_RECV && work->status == W_EOF) {
        // Kill signal from parent
        if (!contr->is_main_sp && work->from == contr->search_pid) {
          close_infd(&contr->pid_fd_table, work->from);
          to_shutdown = true;
        }
        // Main SP 收到 Consultant EOF
        else if (contr->is_main_sp && work->from == contr->search_pid) {
          // 不可以發生！
          // Consultant Manager 的 pipes 在 Main SP 必須保持常開，
          // 直到 Main SP 被終止時才會被回收。
          fprintf(stderr, "Unexpected Consultant Terminated on Node %d\n", contr->pid);
          exit(1);
        }
        // Unexpected Judge Terminated
        else if (work->from == contr->judge_pid) {
          fprintf(stderr, "Unexpected Judge Terminated on Node %d\n", contr->pid);
          to_shutdown = true;
        }
        // Reap child
        else {
          int status;
          if (waitpid(work->from, &status, 0) < 0) {
            fprintf(stderr, "Reap failed: %d", work->from);
          }
          // Close pipe and remove from table
          close_inoutfd(&contr->pid_fd_table, work->from);
          del_pidfdtable(&contr->pid_fd_table, work->from);
        }
      }
      // Unknown
      else {
        fprintf(stderr, "Unknown status for Work: work type '%d' and work status '%d'\n", work->type, work->status);
      }

      free_work(work);
    }

    // 關機
    if (to_shutdown) {
      // 關機計數
      shutdown_loops ++;
      if (shutdown_loops >= MAX_SHUTDOWN_LOOPS) {
        fprintf(stderr, "Max Shutdown Loop Exceeded\n");
        exit(1);
      }

      // 清算依舊存活的 Child (送出 EOF 給所有 child)
      int child_cnt = 0;
      for (int i = 0; i < contr->pid_fd_table.len; i++) {
        pid_t node_pid = contr->pid_fd_table.pids[i];
        if (node_pid == contr->judge_pid || node_pid == contr->search_pid)
          continue;
        close_outfd(&contr->pid_fd_table, node_pid);
        child_cnt ++;
      }
      if (child_cnt == 0) {
        // 已關閉回收所有 child (剩 Search, Judge)
        // => 關閉所有 fds 並退出迴圈
        close_fds_all(&contr->pid_fd_table);
        break;
      }
    }
  }
  free(works_out);

  return 0;
}

void free_controller(Controller *contr) {
  free_key_value_table(contr->kv_table);
  free_route_table(contr->route_table);
  free_worker(contr->worker);
  if (contr->is_main_sp) {
    free_consultant(contr->consult);
  }
}

int main(int argc, char *argv[]) {
  signal(SIGPIPE, SIG_IGN);
  // Parse arguments
  args_t args = argparser(argc, argv);

  // Create controller
  Controller controller;
  init_controller(&controller, args);

  // Start controller
  int ret = run_controller(&controller);

  // Free controller
  free_controller(&controller);

  return ret;
}

bool start_with(char *buf1, size_t len1, char *buf2, size_t len2) {
  if (len1 < len2) return false;
  for (int i = 0; i < len2; i++) {
    if (buf1[i] != buf2[i]) return false;
  }
  return true;
}

// 解析 work->buff 內的指令種類，並將 work ptr 存入 Command 物件後傳回
void parse_command(Work *work, Command *cmd) {
  char *buf = work->buff->buf; size_t len = work->buff->len;
  if (len > 0 && buf[len - 1] == '\n') len--;
  cmd->work = work;
  if (len >= 7 && memcmp(buf, "create ", 7) == 0) {
    cmd->type = CREATE;
  }
  else if (len >= 6 && memcmp(buf, "store ", 6) == 0) {
    cmd->type = STORE;
  }
  else if (len >= 7 && memcmp(buf, "delete ", 7) == 0) {
    cmd->type = DELETE;
  }
  else if (len >= 6 && memcmp(buf, "fetch ", 6) == 0) {
    cmd->type = FETCH;
  }
  else if (len == 4 && memcmp(buf, "kill", 4) == 0) {
    cmd->type = KILL;
  }
  else if (len >= 7 && memcmp(buf, "update ", 7) == 0) {
    cmd->type = UPDATE;
  }
  else {
    cmd->type = UNKNOWN;
  }
}

// Return 0 to continue; Return 1 to break the loop;
// Return -1 for Unknown command.
int command_handler(Controller *contr, const Command *cmd) {
  int ret = 0;
  switch (cmd->type) {
  case CREATE:
    ret = create_handler(contr, cmd->work);
    break;
  case STORE:
    ret = store_handler(contr, cmd->work);
    break;
  case DELETE:
    ret = delete_handler(contr, cmd->work);
    break;
  case FETCH:
    ret = fetch_handler(contr, cmd->work);
    break;
  case KILL:
    ret = kill_handler(contr, cmd->work);
    return 1;
  case UPDATE:
    ret = update_handler(contr, cmd->work);
    break;
  default:
    fprintf(stderr, "Unknown command type\n");
    return -1;
  }
  return ret;
}

/*
  解析 `create <command-fifo> <reply-fifo>` 並執行
*/
int create_handler(Controller *contr, const Work *work) {
  if (work->from != contr->judge_pid) {
    fprintf(stderr, "Create command not from Judge. Ignored.\n");
    return -1;
  }

  // 解析指令
  Buff **out = (Buff **)malloc(sizeof(Buff *) * 3);
  size_t n_out = 3;
  int n_buff = split(work->buff, ' ', &out, &n_out);
  if (n_buff != 3) {
    fprintf(stderr, "Invalid split buff count: %d\n", n_buff);
    for (int i = 0; i < n_buff; i++) {
      free_buff(out[i]);
    }
    free(out);
    return -1;
  }

  // 建立子節點溝通 pipe
  // fds1[0]: parent read, fds1[1]: child write
  // fds2[0]: child read, fds2[1]: parent write
  int fds1[2], fds2[2];
  pipe(fds1); pipe(fds2);
  // Set Nonblocking
  set_nonblocking(fds1[0]);
  set_nonblocking(fds1[1]);
  set_nonblocking(fds2[0]);
  set_nonblocking(fds2[1]);

  // 執行子節點
  pid_t cpid;
  if ((cpid = fork()) == 0) {
    close(fds1[0]); close(fds2[1]);
    close_fds_all(&contr->pid_fd_table);

    char *cmd_fifo = out[1]->buf;
    char *rply_fifo = out[2]->buf;
    cmd_fifo[out[1]->len] = '\0'; rply_fifo[out[2]->len] = '\0'; // 補結尾
    char pifd[12], pofd[12], jpid[12];
    snprintf(pifd, sizeof(pifd), "%d", fds2[0]);
    snprintf(pofd, sizeof(pofd), "%d", fds1[1]);
    snprintf(jpid, sizeof(jpid), "%d", contr->judge_pid);

    execl("./sp", cmd_fifo, rply_fifo, pifd, pofd, jpid, NULL);
    fprintf(stderr, "execl failed\n");
    exit(1);
  } else {
    close(fds1[1]); close(fds2[0]);
    add_pidfdtable(&contr->pid_fd_table, cpid, fds1[0], fds2[1]);
  }

  // Free
  for (int i = 0; i < n_buff; i++) {
    free_buff(out[i]);
  }
  free(out);
  return 0;
}

/*
  解析 `store k v` 並執行
*/
int store_handler(Controller *contr, const Work *work) {
  const Buff *line = work->buff;
  size_t end = line->len;
  if (end && line->buf[end - 1] == '\n') end--;
  size_t key_start = 6, key_end = key_start;
  while (key_end < end && line->buf[key_end] != ' ') key_end++;
  if (key_end == key_start || key_end >= end || key_end + 1 >= end) return -1;
  Buff *key = create_buff(), *value = create_buff();
  write_buff(key, line->buf + key_start, key_end - key_start);
  write_buff(value, line->buf + key_end + 1, end - key_end - 1);
  if (store(contr->kv_table, key, value) < 0) {
    fprintf(stderr, "Store Failed.\n");
    free_buff(key); free_buff(value);
    return -1;
  }
  free_buff(key); free_buff(value);
  return 0;
}

/*
  解析 `delete k` 並執行
*/
int delete_handler(Controller *contr, const Work *work) {
  // Extract command
  Buff **out = (Buff **)malloc(sizeof(Buff *) * 2);
  size_t n_out = 2;
  int n_buff = split(work->buff, ' ', &out, &n_out);
  if (n_buff != 2) {
    fprintf(stderr, "Invalid store command length: %d\n", n_buff);
    for (int i = 0; i < n_buff; i++) {
      free_buff(out[i]);
    }
    free(out);
    return -1;
  }

  if (delete_(contr->kv_table, out[1]) < 0) {
    fprintf(stderr, "Delete Failed.\n");
    for (int i = 0; i < n_buff; i++) {
      free_buff(out[i]);
    }
    free(out);
    return -1;
  }

  // Free
  for (int i = 0; i < n_buff; i++) {
    free_buff(out[i]);
  }
  free(out);
  return 0;
}

/*
  解析 `fetch <hash> <key>` 並執行
*/
int fetch_handler(Controller *contr, const Work *work) {
  // Extrace command
  Buff **out = (Buff **)malloc(sizeof(Buff *) * 3);
  size_t n_out = 3;
  int n_buff = split(work->buff, ' ', &out, &n_out);
  if (n_buff != 3) {
    fprintf(stderr, "Invalid fetch command length.\n");
    for (int i = 0; i < n_buff; i++) {
      free_buff(out[i]);
    }
    free(out);
    return -1;
  }

  // Case 1: key is in the receiving node
  Buff *value;
  if ((value = fetch(contr->kv_table, out[2])) != NULL) {
    // Send back to previous node (may be Judge)
    Buff *buff = create_buff();
    copy_buff(buff, out[1]); // hase
    write_buff(buff, " ", 1); // space
    write_buff(buff, value->buf, value->len); // value
    write_buff(buff, "\n", 1); // EOL
    Work *new_work = create_work(contr->pid, work->from, W_SEND, buff);
    push_work(contr->worker, new_work);
  }
  else if (contr->is_main_sp) {
    value = consultant_lookup(contr->consult, out[2]);
    if (value == NULL) {
      fprintf(stderr, "Consultant lookup failed\n");
    } else {
      Buff *reply = create_buff();
      copy_buff(reply, out[1]);
      write_buff(reply, " ", 1);
      write_buff(reply, value->buf, value->len);
      write_buff(reply, "\n", 1);
      push_work(contr->worker, create_work(contr->pid, work->from, W_SEND, reply));
      free_buff(value);
    }
  }
  // Case 2: the key is in an ancestor or at consultant
  else {
    // 建立向上搜尋工作
    Buff *buff = create_buff();
    copy_buff(buff, work->buff);
    Work *new_work = create_work(contr->pid, contr->search_pid, W_SEND, buff);
    push_work(contr->worker, new_work);
    // 存入路由表
    add_route_table(contr->route_table, work->from, out[1]);
  }

  // Free
  for (int i = 0; i < n_buff; i++) {
    free_buff(out[i]);
  }
  free(out);
  return 0;
}

int kill_handler(Controller *contr, const Work *work) {
  // 不做任何事，command handler 回傳 1，
  // 直接在 Loop 內設置 to_shutdown
  // 讓 Loop 自行處理關機流程
  return 0;
}

/*
  解析 `update <new-consultant-path>` 並執行
*/
int update_handler(Controller *contr, const Work *work) {
  if (work->from != contr->judge_pid) {
    fprintf(stderr, "Update command not from Judge. Ignored.\n");
    return -1;
  }
  if (!contr->is_main_sp) {
    fprintf(stderr, "Normal node does not accept update command. Ignored.\n");
    return -1;
  }

  // Extract command
  Buff **out = (Buff **)malloc(sizeof(Buff *) * 2);
  size_t n_out = 2;
  int n_buff = split(work->buff, ' ', &out, &n_out);
  if (n_buff != 2) {
    fprintf(stderr, "Invalid update command length: %d\n", n_buff);
    for (int i = 0; i < n_buff; i++) {
      free_buff(out[i]);
    }
    free(out);
    return -1;
  }

  char *path = malloc(out[1]->len + 1);
  memcpy(path, out[1]->buf, out[1]->len);
  path[out[1]->len] = '\0';
  struct stat st;
  if (stat(path, &st) == 0 && S_ISREG(st.st_mode) && access(path, X_OK) == 0)
    update_consultant(contr->consult, out[1]);
  free(path);

  // Free
  for (int i = 0; i < n_buff; i++) {
    free_buff(out[i]);
  }
  free(out);
  return 0;
}
