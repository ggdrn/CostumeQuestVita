/* MIT license; see LICENSE. */
#include "utils/log_queue.h"
#include <string.h>
int log_queue_append(LogQueue *q, const void *data, size_t len) {
    if (len > LOG_QUEUE_CAPACITY - q->count) { q->dropped++; return 0; }
    size_t tail = (q->head + q->count) % LOG_QUEUE_CAPACITY;
    size_t first = LOG_QUEUE_CAPACITY - tail;
    if (first > len) first = len;
    memcpy(q->data + tail, data, first);
    memcpy(q->data, (const uint8_t *)data + first, len - first);
    q->count += len;
    return 1;
}
size_t log_queue_take(LogQueue *q, void *out, size_t cap) {
    size_t len = q->count < cap ? q->count : cap;
    size_t first = LOG_QUEUE_CAPACITY - q->head;
    if (first > len) first = len;
    memcpy(out, q->data + q->head, first);
    memcpy((uint8_t *)out + first, q->data, len - first);
    q->head = (q->head + len) % LOG_QUEUE_CAPACITY;
    q->count -= len;
    return len;
}
