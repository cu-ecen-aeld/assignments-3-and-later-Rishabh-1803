#include "threading.h"

#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>

// Optional debug prints
#define DEBUG_LOG(msg,...)
// #define DEBUG_LOG(msg,...) printf("threading: " msg "\n", ##__VA_ARGS__)

#define ERROR_LOG(msg,...)
// #define ERROR_LOG(msg,...) printf("threading ERROR: " msg "\n", ##__VA_ARGS__)


void* threadfunc(void *thread_param)
{
    thread_data_t *data =
            (thread_data_t *)thread_param;

    if(data == NULL)
    {
        return thread_param;
    }

    data->thread_complete_success = false;

    /* Wait before locking mutex */
    usleep(data->wait_to_obtain_ms * 1000);

    /* Lock mutex */
    if(pthread_mutex_lock(data->mutex) != 0)
    {
        return thread_param;
    }

    /* Hold mutex */
    usleep(data->wait_to_release_ms * 1000);

    /* Unlock mutex */
    if(pthread_mutex_unlock(data->mutex) != 0)
    {
        return thread_param;
    }

    data->thread_complete_success = true;

    return thread_param;
}


bool start_thread_obtaining_mutex(
        pthread_t *thread,
        pthread_mutex_t *mutex,
        int wait_to_obtain_ms,
        int wait_to_release_ms)
{
    thread_data_t *data =
            malloc(sizeof(thread_data_t));

    if(data == NULL)
    {
        return false;
    }

    data->mutex = mutex;
    data->wait_to_obtain_ms = wait_to_obtain_ms;
    data->wait_to_release_ms = wait_to_release_ms;
    data->thread_complete_success = false;

    int ret = pthread_create(
                    thread,
                    NULL,
                    threadfunc,
                    data);

    if(ret != 0)
    {
        free(data);
        return false;
    }

    return true;
}
