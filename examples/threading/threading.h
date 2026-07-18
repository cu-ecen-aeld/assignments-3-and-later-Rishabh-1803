#ifndef THREADING_H
#define THREADING_H

#include <stdbool.h>
#include <pthread.h>

typedef struct thread_data
{
    pthread_mutex_t *mutex;
    int wait_to_obtain_ms;
    int wait_to_release_ms;

    bool thread_complete_success;
} thread_data_t;

/**
 * Start a thread which:
 * 1. Waits wait_to_obtain_ms
 * 2. Obtains mutex
 * 3. Waits wait_to_release_ms
 * 4. Releases mutex
 */
bool start_thread_obtaining_mutex(
        pthread_t *thread,
        pthread_mutex_t *mutex,
        int wait_to_obtain_ms,
        int wait_to_release_ms);

#endif
