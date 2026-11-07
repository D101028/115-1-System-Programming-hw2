#ifndef QUEUE_H
#define QUEUE_H

#include <stdbool.h>

// Queue 的節點結構
typedef struct QueueNode {
  void *data;
  struct QueueNode *next;
} QueueNode;

// Queue 控制結構
typedef struct {
  QueueNode *front;           // 指向隊頭（出隊端）
  QueueNode *rear;            // 指向隊尾（入隊端）
  int size;                   // 目前 Queue 的大小
} Queue;

Queue* createQueue();
bool isEmpty(Queue *q);
void enqueue(Queue *q, void *data);
void* dequeue(Queue *q);
void* peek(Queue *q);
void* peekRear(Queue *q);
int getSize(Queue *q);
void freeQueue(Queue *q, void (*free_data_cb)(void*));

#endif