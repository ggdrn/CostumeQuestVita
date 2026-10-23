/* Host: cc -Wall -Wextra -Werror -Isource tests/log_queue_test.c
   source/utils/log_queue.c -o /tmp/cq-log-queue-test && /tmp/cq-log-queue-test */
#include "utils/log_queue.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static LogQueue queue;
static unsigned char expected[LOG_QUEUE_CAPACITY], actual[LOG_QUEUE_CAPACITY];
int main(void) {
    for (size_t i = 0; i < sizeof(expected); i++) expected[i] = i % 251;
    // A slow card does not stop producers: a full buffer drops new records.
    assert(log_queue_append(&queue, expected, sizeof(expected)));
    assert(!log_queue_append(&queue, "dropped\n", 8));
    assert(queue.dropped == 1);
    assert(log_queue_take(&queue, actual, 12345) == 12345);
    assert(memcmp(actual, expected, 12345) == 0);
    // Refill while the old batch is being written: ring wraps, order survives.
    assert(log_queue_append(&queue, expected, 12345));
    assert(log_queue_take(&queue, actual, sizeof(actual)) == sizeof(actual));
    assert(memcmp(actual, expected + 12345, sizeof(actual) - 12345) == 0);
    assert(memcmp(actual + sizeof(actual) - 12345, expected, 12345) == 0);
    assert(log_queue_take(&queue, actual, sizeof(actual)) == 0);
    for (int i = 0; i < 5000; i++) {
        const char *line = "[    1234] main thread diagnostic\n";
        size_t n = strlen(line);
        assert(log_queue_append(&queue, line, n));
        assert(log_queue_take(&queue, actual, sizeof(actual)) == n);
        assert(memcmp(actual, line, n) == 0);
    }
    puts("Log queue: overflow, wrap, ordering and repeated batching passed");
}
