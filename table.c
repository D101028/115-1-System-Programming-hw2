#include "table.h"
#include "buffer.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

// Create the key-value table with size of DEFAULT_KEY_VALUE_TABLE_SIZE
KeyValueTable *create_key_value_table() {
  KeyValueTable *table = (KeyValueTable *)malloc(sizeof(KeyValueTable));
  table->key_arr = (Buff **)malloc(sizeof(Buff *) * DEFAULT_KEY_VALUE_TABLE_SIZE);
  table->value_arr = (Buff **)malloc(sizeof(Buff *) * DEFAULT_KEY_VALUE_TABLE_SIZE);
  table->len = 0;
  table->size = DEFAULT_KEY_VALUE_TABLE_SIZE;
  return table;
}
int store(KeyValueTable *table, const Buff *key_buff, const Buff *value_buff) {
  bool new = true;
  for (int i = 0; i < table->len; i++) {
    if (buff_eq(table->key_arr[i], key_buff, false)) {
      // Update value
      copy_buff(table->value_arr[i], value_buff);
      new = false;
      break;
    }
  }
  if (new) {
    if (table->len >= table->size * 0.7) {
      size_t new_size = table->size * 2;
      Buff **keys = realloc(table->key_arr, sizeof(Buff *) * new_size);
      if (keys == NULL) return -1;
      table->key_arr = keys;
      Buff **values = realloc(table->value_arr, sizeof(Buff *) * new_size);
      if (values == NULL) return -1;
      table->value_arr = values;
      table->size = new_size;
    }
    Buff *key = create_buff();
    Buff *value = create_buff();
    copy_buff(key, key_buff);
    copy_buff(value, value_buff);
    table->key_arr[table->len] = key;
    table->value_arr[table->len] = value;
    table->len++;
  }
  return 0;
}
// Return 0 for del succeeded; return -1 for not found
int delete_(KeyValueTable *table, const Buff *key_buff) {
  if (table->len <= 0) return -1;
  for (int i = 0; i < table->len; i++) {
    if (buff_eq(table->key_arr[i], key_buff, true)) {
      table->len --;
      size_t len = table->len;
      // Remove
      free_buff(table->key_arr[i]);
      free_buff(table->value_arr[i]);
      // Move
      if ((size_t)i != len) {
        table->key_arr[i] = table->key_arr[len];
        table->value_arr[i] = table->value_arr[len];
      }
      // Delete
      table->key_arr[len] = NULL;
      table->value_arr[len] = NULL;
      return 0;
    }
  }
  return -1;
}
// 回傳 table 內的 value 指標，找不到者回傳 NULL
// 若必要，請自行複製一份，防止 table 內的 buffer 被意外地更改或釋放
Buff *fetch(KeyValueTable *table, const Buff *key_buff) {
  for (int i = 0; i < table->len; i++) {
    if (buff_eq(table->key_arr[i], key_buff, false)) return table->value_arr[i];
  }
  return NULL;
}
// 一併清除 key_arr, value_arr 內的所有 Buff
void free_key_value_table(KeyValueTable *table) {
  for (int i = 0; i < table->len; i++) {
    free_buff(table->key_arr[i]);
    free_buff(table->value_arr[i]);
  }
  free(table->key_arr);
  free(table->value_arr);
  free(table);
}

void init_pidfdtable(PidFDTable *table) {
  table->len = 0;
  for (int i = 0; i < MAX_FD_PID_PAIRS; i++) {
    table->pids[i] = NULL_PID;
    table->in_fds[i] = NULL_FD;
    table->out_fds[i] = NULL_FD;
  }
}
int add_pidfdtable(PidFDTable *table, pid_t pid, int in_fd, int out_fd) {
  if (table->len >= MAX_FD_PID_PAIRS) {
    fprintf(stderr, "Out of PidFDTable size\n");
    return -1;
  }
  size_t i = table->len;
  table->pids[i] = pid;
  table->in_fds[i] = in_fd;
  table->out_fds[i] = out_fd;
  table->len ++;
  return 0;
}
// Search the fds, DOES NOT close them, and remove the record from table.
// Return 0 for del succeeded; return 1 for not found; return -1 for failed
int del_pidfdtable(PidFDTable *table, pid_t pid) {
  if (table->len <= 0) return -1;
  int del_pos = -1;
  for (int i = 0; i < table->len; i++) {
    if (table->pids[i] == pid) {
      del_pos = i;
      break;
    }
  }
  if (del_pos >= 0) {
    table->len --;
    size_t len = table->len;
    // Move
    table->in_fds[del_pos] = table->in_fds[len];
    table->out_fds[del_pos] = table->out_fds[len];
    table->pids[len] = table->pids[len];
    // Delete (Optional)
    table->in_fds[len] = NULL_FD;
    table->out_fds[len] = NULL_FD;
    table->pids[len] = NULL_PID;
    return 0;
  } else {
    return 1;
  }
}
// Return NULL_PID (0) for not found
pid_t search_pid_in(const PidFDTable *table, int fd) {
  for (int i = 0; i < table->len; i++) {
    if (table->out_fds[i] == fd)
      return table->pids[i];
  }
  return NULL_PID;
}
// Return NULL_PID (0) for not found
pid_t search_pid_out(const PidFDTable *table, int fd) {
  for (int i = 0; i < table->len; i++) {
    if (table->in_fds[i] == fd)
      return table->pids[i];
  }
  return NULL_PID;
}
// Return NULL_PID (0) for not found
pid_t search_pid_inout(const PidFDTable *table, int fd) {
  for (int i = 0; i < table->len; i++) {
    if (table->in_fds[i] == fd || table->out_fds[i] == fd)
      return table->pids[i];
  }
  return NULL_PID;
}
// Return NULL_FD (-1) for not found
int search_infd(const PidFDTable *table, pid_t pid) {
  for (int i = 0; i < table->len; i++) {
    if (table->pids[i] == pid)
      return table->in_fds[i];
  }
  return NULL_FD;
}
// Return NULL_FD (-1) for not found
int search_outfd(const PidFDTable *table, pid_t pid) {
  for (int i = 0; i < table->len; i++) {
    if (table->pids[i] == pid)
      return table->out_fds[i];
  }
  return NULL_FD;
}
int close_infd(PidFDTable *table, pid_t pid) {
  for (int i = 0; i < table->len; i++) {
    if (table->pids[i] == pid) {
      close(table->in_fds[i]);
      table->in_fds[i] = NULL_FD;
      return 0;
    }
  }
  return -1;
}
int close_outfd(PidFDTable *table, pid_t pid) {
  for (int i = 0; i < table->len; i++) {
    if (table->pids[i] == pid) {
      close(table->out_fds[i]);
      table->out_fds[i] = NULL_FD;
      return 0;
    }
  }
  return -1;
}
int close_inoutfd(PidFDTable *table, pid_t pid) {
  for (int i = 0; i < table->len; i++) {
    if (table->pids[i] == pid) {
      close(table->in_fds[i]);
      close(table->out_fds[i]);
      table->in_fds[i] = NULL_FD;
      table->out_fds[i] = NULL_FD;
      return 0;
    }
  }
  return -1;
}
int close_fds_all(PidFDTable *table) {
  for (int i = 0; i < table->len; i++) {
    if (table->in_fds[i] >= 0) {
      close(table->in_fds[i]);
      table->in_fds[i] = NULL_FD;
    }
    if (table->out_fds[i] >= 0) {
      close(table->out_fds[i]);
      table->out_fds[i] = NULL_FD;
    }
  }
  return 0;
}

// Create the route table with size of DEFAULT_ROUTE_TABLE_SIZE
RouteTable *create_route_table() {
  RouteTable *table = (RouteTable *)malloc(sizeof(RouteTable));
  table->hash_arr = (Buff **)malloc(sizeof(Buff *) * DEFAULT_ROUTE_TABLE_SIZE);
  table->pid_arr = (pid_t *)malloc(sizeof(pid_t) * DEFAULT_ROUTE_TABLE_SIZE);
  table->len = 0;
  table->size = DEFAULT_ROUTE_TABLE_SIZE;
  return table;
}
int add_route_table(RouteTable *table, pid_t pid, const Buff *hash) {
  Buff *buff = create_buff();
  copy_buff(buff, hash);
  table->hash_arr[table->len] = buff;
  table->pid_arr[table->len] = pid;
  table->len ++;
  if (table->len >= table->size * 0.7) {
    table->hash_arr = realloc(table->hash_arr, sizeof(Buff *) * table->size * 2);
    table->pid_arr = realloc(table->pid_arr, sizeof(pid_t) * table->size * 2);
    table->size *= 2;
  }
  return 0;
}
int del_route_table_by_hash(RouteTable *table, const Buff *hash) {
  for (int i = 0; i < table->len; i++) {
    if (buff_eq(table->hash_arr[i], hash, false))  {
      table->len--;
      // Remove
      free_buff(table->hash_arr[i]);
      // Move
      table->hash_arr[i] = table->hash_arr[table->len];
      table->pid_arr[i] = table->pid_arr[table->len];
      // Delete
      table->hash_arr[table->len] = NULL;
      table->pid_arr[table->len] = NULL_PID;
      return 0;
    }
  }
  return -1;
}
pid_t search_pid_by_hash(const RouteTable *table, const Buff *hash) {
  for (int i = 0; i < table->len; i++) {
    if (buff_eq(table->hash_arr[i], hash, false)) {
      return table->pid_arr[i];
    }
  }
  return NULL_PID;
}
void free_route_table(RouteTable *table) {
  for (int i = 0; i < table->len; i++) {
    free_buff(table->hash_arr[i]);
  }
  free(table->hash_arr);
  free(table->pid_arr);
  free(table);
}
