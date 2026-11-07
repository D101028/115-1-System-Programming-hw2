#ifndef BUFFER_H
#define BUFFER_H

#include <stdio.h>
#include <stdbool.h>

#define DEFAULT_BUFF_SIZE 1024

typedef struct {
  char *buf;
  size_t len;   // 目前 buf 用量
  size_t size;  // 目前 buf 長度上限
  size_t left;  // 目前讀取頭位置
} Buff;

Buff *create_buff();
int write_buff(Buff *buff, const char *buf, size_t len);
void clear_buff(Buff *buff);
void free_buff(Buff *buff);
bool buff_eq(const Buff *buff1, const Buff *buff2, bool weak_cmp);
void copy_buff(Buff *dest, const Buff *src);

int find_char(const Buff *buff, char target, size_t start);
int split(const Buff *buff, char sep, Buff ***out, size_t *n_out);

#endif