/* MIT license; see LICENSE. */
#ifndef CQ_LOG_QUEUE_H
#define CQ_LOG_QUEUE_H
#include <stddef.h>
#include <stdint.h>
#define LOG_QUEUE_CAPACITY (64 * 1024)
typedef struct {
    uint8_t data[LOG_QUEUE_CAPACITY];
    size_t head, count;
    uint32_t dropped;
} LogQueue;
/* Caller supplies synchronization; complete records are accepted or dropped. */
int log_queue_append(LogQueue *q, const void *data, size_t len);
size_t log_queue_take(LogQueue *q, void *out, size_t cap);
#endif
