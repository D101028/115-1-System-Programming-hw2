#ifndef TABLE_H
#define TABLE_H

#include "buffer.h"
#include <stdio.h>
#include <sys/types.h>

#define DEFAULT_KEY_VALUE_TABLE_SIZE 64
#define MAX_FD_PID_PAIRS 32
#define DEFAULT_ROUTE_TABLE_SIZE 32
#define NULL_PID 0
#define NULL_FD (-1)

/*
  動態長度清單，紀錄此節點存放之 key-value pairs
*/
typedef struct {
  Buff **key_arr;
  Buff **value_arr;
  size_t len;
  size_t size;
} KeyValueTable;

KeyValueTable *create_key_value_table();
int store(KeyValueTable *table, const Buff *key_buff, const Buff *value_buff);
int delete_(KeyValueTable *table, const Buff *key_buff);
Buff *fetch(KeyValueTable *table, const Buff *key_buff);
void free_key_value_table(KeyValueTable *table);

/*
  固定長度清單，紀錄 PID 與 in_fd, out_fd 的對應關係
  映射關係一一對應且不重複映射。
*/
typedef struct {
  pid_t pids[MAX_FD_PID_PAIRS];
  int in_fds[MAX_FD_PID_PAIRS];
  int out_fds[MAX_FD_PID_PAIRS];
  size_t len;
} PidFDTable;

void init_pidfdtable(PidFDTable *table);
int add_pidfdtable(PidFDTable *table, pid_t pid, int in_fd, int out_fd);
int del_pidfdtable(PidFDTable *table, pid_t pid);
pid_t search_pid_in(const PidFDTable *table, int fd);
pid_t search_pid_out(const PidFDTable *table, int fd);
pid_t search_pid_inout(const PidFDTable *table, int fd);
int search_infd(const PidFDTable *table, pid_t pid);
int search_outfd(const PidFDTable *table, pid_t pid);
int close_infd(PidFDTable *table, pid_t pid);
int close_outfd(PidFDTable *table, pid_t pid);
int close_inoutfd(PidFDTable *table, pid_t pid);
int close_fds_all(PidFDTable *table);

/*
  動態長度清單，紀錄 hash 與發送者 PID 之映射關係
*/
typedef struct {
  Buff **hash_arr;
  pid_t *pid_arr;
  size_t len;
  size_t size;
} RouteTable;

RouteTable *create_route_table();
int add_route_table(RouteTable *table, pid_t pid, const Buff *hash);
int del_route_table_by_hash(RouteTable *table, const Buff *hash);
pid_t search_pid_by_hash(const RouteTable *table, const Buff *hash);
void free_route_table(RouteTable *table);

#endif