#include "buffer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

inline static size_t min(size_t a, size_t b) { return (a < b) ? a : b; }

// Create a Buff with size of DEFAULT_BUFF_SIZE
Buff *create_buff() {
  Buff *buff = (Buff *)malloc(sizeof(Buff));
  char *buf = malloc(sizeof(char) * DEFAULT_BUFF_SIZE);
  buff->buf = buf;
  buff->len = 0;
  buff->size = DEFAULT_BUFF_SIZE;
  buff->left = 0;
  return buff;
}

void resize_buff(Buff *buff, size_t size) {
  buff->buf = realloc(buff->buf, size);
  buff->size = size;
}

int write_buff(Buff *buff, const char *buf, size_t len) {
  if (buff->len + len >= buff->size * 0.7) {
    // Resize the buffer
    size_t required = buff->len + len;
    size_t new_size = buff->size << 1;
    while (new_size <= required) new_size <<= 1; // * 2
    resize_buff(buff, new_size);
  }
  memcpy(buff->buf + buff->len, buf, len);
  buff->len += len;
  return len;
}

// int read_buff(Buff *buff, char *buf, size_t len) {
//   if (buff->left > buff->len) {
//     fprintf(stderr, "讀取頭位置錯誤：left=%d, len=%d\n", (int)buff->left, (int)buff->len);
//     return -1;
//   }
//   if (buff->left + len < buff->len) {
//     memcpy(buf, buff->buf, len);
//     return len;
//   } else {
//     memcpy(buf, buff->buf, buff->len - buff->left);
//     return buff->len - buff->left;
//   }
// }

void clear_buff(Buff *buff) {
  buff->len = 0;
}

void free_buff(Buff *buff) {
  free(buff->buf);
  free(buff);
}

bool buff_eq(const Buff *buff1, const Buff *buff2, bool weak_cmp) {
  if (buff1 == NULL || buff2 == NULL) return false;
  if (!weak_cmp && buff1->len != buff2->len) return false;
  size_t n = min(buff1->len, buff2->len);
  return memcmp(buff1->buf, buff2->buf, n) == 0;
}

// Clear dest and copy content from src
void copy_buff(Buff *dest, const Buff *src) {
  clear_buff(dest);
  write_buff(dest, src->buf, src->len);
}

// 從 start 開始，遍歷 buffer, 找到 target 回傳 index
// Return -1 if not found
int find_char(const Buff *buff, char target, size_t start) {
  for (size_t i = start; i < buff->len; i++) {
    if (buff->buf[i] == target) return i;
  }
  return -1;
}

// 將 buff 以 sep 切割，自動 create 新的 Buff，放入 out
// 若 out 分配的長度不足，自動 realloc
// 回傳切割後的 buff 數量
// 此函數會自動忽略開頭與結尾的 `\n` & sep
int split(const Buff *buff, char sep, Buff ***out, size_t *n_out) {
    // 防呆檢查：如果 buff 為空或長度為 0，直接回傳 0
    if (!buff || !buff->buf || buff->len == 0) {
        return 0;
    }

    char *buf = buff->buf;
    int start = 0;
    int end = buff->len - 1;

    // 略過開頭的 \n 與 sep
    while (start <= end && (buf[start] == '\n' || buf[start] == sep)) {
        start++;
    }

    // 略過結尾的 \n 與 sep
    while (end >= start && (buf[end] == '\n' || buf[end] == sep)) {
        end--;
    }

    // 如果字串裡全都是 \n 或 sep，過濾後 start 會大於 end，直接回傳 0
    if (start > end) {
        return 0;
    }

    int cnt = 0;
    size_t len = 0;
    size_t head = start; // 從有效起點開始計算

    // 開始切割，範圍只在有效的 start 到 end 之間
    // 這裡條件設為 i <= end + 1，用來處理最後一個區塊的收尾
    for (int i = start; i <= end + 1; i++) {
        // 當遇到分隔符號，或是到達有效字串的結尾 (end + 1) 時，進行切割
        if (i == end + 1 || buf[i] == sep) {

            // realloc 擴充陣列邏輯
            if (cnt >= *n_out) {
                *n_out = (*n_out == 0) ? 4 : (*n_out) * 2;
                *out = (Buff **)realloc(*out, (*n_out) * sizeof(Buff *));
                if (*out == NULL) {
                    return -1; // 記憶體分配失敗
                }
            }

            Buff *sub_buff = create_buff();
            write_buff(sub_buff, buf + head, len);
            (*out)[cnt] = sub_buff;
            cnt++;

            head = i + 1;
            len = 0;
        } else {
            len++;
        }
    }

    return cnt;
}

