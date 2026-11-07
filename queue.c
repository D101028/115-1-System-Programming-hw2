#include "queue.h"

#include <stdlib.h>
#include <stdbool.h>

// 初始化 Queue
Queue* createQueue() {
  Queue *q = (Queue*)malloc(sizeof(Queue));
  if (!q) return NULL;
  q->front = q->rear = NULL;
  q->size = 0;
  return q;
}

// 檢查是否為空
bool isEmpty(Queue *q) {
  return (q == NULL || q->size == 0);
}

// 入隊 (Enqueue)
void enqueue(Queue *q, void *data) {
  if (!q) return;
  
  QueueNode *newNode = (QueueNode*)malloc(sizeof(QueueNode));
  if (!newNode) return;
  
  newNode->data = data;
  newNode->next = NULL;
  
  if (q->rear == NULL) {      // 如果 Queue 是空的
      q->front = q->rear = newNode;
  } else {
      q->rear->next = newNode;
      q->rear = newNode;
  }
  q->size++;
}

// 出隊 (Dequeue)
void* dequeue(Queue *q) {
  if (isEmpty(q)) return NULL;
  
  QueueNode *temp = q->front;
  void *data = temp->data;    // 取出資料指標
  
  q->front = q->front->next;
  if (q->front == NULL) {     // 如果移出後變空了
      q->rear = NULL;
  }
  
  free(temp);
  q->size--;
  return data;
}

// 查看隊頭元素 (Peek)
void* peek(Queue *q) {
  if (isEmpty(q)) return NULL;
  return q->front->data;
}
// 查看對尾元素
void* peekRear(Queue *q) {
  if (isEmpty(q)) return NULL;
  return q->rear->data;
}

// 取得 Queue 大小
int getSize(Queue *q) {
  return q ? q->size : 0;
}

// 釋放 Queue 記憶體 (free_data_cb 是用來釋放使用者自訂資料的 callback 函式)
void freeQueue(Queue *q, void (*free_data_cb)(void*)) {
  if (!q) return;
  while (!isEmpty(q)) {
    void *data = dequeue(q);
    if (free_data_cb && data) {
      free_data_cb(data); // 釋放使用者配置的資料記憶體
    }
  }
  free(q);
}
