/*
 * aesdchar.h
 *
 * AESD character driver definitions.
 */

#ifndef AESD_CHAR_DRIVER_AESDCHAR_H_
#define AESD_CHAR_DRIVER_AESDCHAR_H_

#define AESD_DEBUG 1

#undef PDEBUG
#ifdef AESD_DEBUG
#  ifdef __KERNEL__
#    define PDEBUG(fmt, args...) printk(KERN_DEBUG "aesdchar: " fmt, ##args)
#  else
#    define PDEBUG(fmt, args...) fprintf(stderr, fmt, ##args)
#  endif
#else
#  define PDEBUG(fmt, args...)
#endif

#ifdef __KERNEL__
#include <linux/cdev.h>
#include <linux/mutex.h>
#include <linux/types.h>

#include "aesd-circular-buffer.h"

struct aesd_dev
{
    struct cdev cdev;
    struct aesd_circular_buffer buffer;
    struct mutex lock;

    /*
     * Data from write() calls which has not yet reached '\n'.
     */
    char *write_buffer;
    size_t write_buffer_size;
};
#endif

#endif /* AESD_CHAR_DRIVER_AESDCHAR_H_ */
