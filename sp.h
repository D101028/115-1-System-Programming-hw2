#ifndef SP_H
#define SP_H

#include "table.h"
#include "buffer.h"
#include "queue.h"
#include "worker.h"
#include <stdio.h>
#include <stdbool.h>

#define DEFAULT_WORKS_NUM 32
#define FAKE_PID (-1)
#define MAX_SHUTDOWN_LOOPS 32

typedef struct {
  bool is_main_sp;
  char *cmd_path;
  char *rply_path;
  int search_in_fd;
  int search_out_fd;
  pid_t judge_pid;
} args_t;

args_t argparser(int argc, char *argv[]);

int create_fifos_at_stdio(char *cmd_path, char *rply_path);

typedef struct {
  bool is_main_sp;
  pid_t pid;
  KeyValueTable *kv_table;
  PidFDTable pid_fd_table;
  RouteTable *route_table;
  pid_t judge_pid;
  pid_t search_pid;
  Worker *worker;
  Consultant *consult; // NULL by default (except Main SP)
} Controller;

Consultant *create_consultant(int fds[2]);
void update_consultant(Consultant *consult, const Buff *new_path);
void run_consultant(Controller *contr, Work *work);
void free_consultant(Consultant *consult);

void init_controller(Controller *contr, args_t args);
int run_controller(Controller *controller);
void free_controller(Controller *controller);

typedef struct {
  Work *work;
  enum { CREATE, STORE, DELETE, FETCH, KILL, UPDATE, UNKNOWN } type;
} Command;

void parse_command(Work *work, Command *cmd);
int command_handler(Controller *contr, const Command *cmd);
int create_handler(Controller *contr, const Work *work);
int store_handler(Controller *contr, const Work *work);
int delete_handler(Controller *contr, const Work *work);
int fetch_handler(Controller *contr, const Work *work);
int kill_handler(Controller *contr, const Work *work);
int update_handler(Controller *contr, const Work *work);

#endif // SP_H