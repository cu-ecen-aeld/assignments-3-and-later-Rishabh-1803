/**
 * @file main.c
 * @brief AESD character driver implementation
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/errno.h>
#include <linux/kernel.h>

#include "aesdchar.h"
#include "aesd_ioctl.h"

int aesd_major = 0;
int aesd_minor = 0;

MODULE_AUTHOR("Rishabh Shah");
MODULE_LICENSE("Dual BSD/GPL");

struct aesd_dev aesd_device;

/**
 * Return the total number of bytes currently stored in the circular buffer.
 * Caller must hold aesd_device.lock.
 */
static size_t aesd_buffer_size(struct aesd_circular_buffer *buffer)
{
    size_t total = 0;
    uint8_t index;
    uint8_t count;

    if (buffer->full)
        count = AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
    else
        count = buffer->in_offs;

    for (index = 0; index < count; index++) {
        uint8_t entry_index =
            (buffer->out_offs + index) %
            AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;

        total += buffer->entry[entry_index].size;
    }

    return total;
}

/**
 * Free an entry which is about to be overwritten.
 * Caller must hold aesd_device.lock.
 */
static void aesd_free_overwritten_entry(struct aesd_circular_buffer *buffer)
{
    if (buffer->full && buffer->entry[buffer->in_offs].buffptr) {
        kfree(buffer->entry[buffer->in_offs].buffptr);
        buffer->entry[buffer->in_offs].buffptr = NULL;
        buffer->entry[buffer->in_offs].size = 0;
    }
}

int aesd_open(struct inode *inode, struct file *filp)
{
    PDEBUG("open\n");

    filp->private_data = &aesd_device;

    return 0;
}

int aesd_release(struct inode *inode, struct file *filp)
{
    PDEBUG("release\n");

    return 0;
}

ssize_t aesd_read(struct file *filp, char __user *buf, size_t count,
                  loff_t *f_pos)
{
    struct aesd_dev *dev = filp->private_data;
    struct aesd_buffer_entry *entry;
    size_t entry_offset = 0;
    size_t available;
    size_t bytes_to_copy;
    ssize_t retval = 0;

    PDEBUG("read %zu bytes with offset %lld\n", count, *f_pos);

    if (count == 0)
        return 0;

    if (*f_pos < 0)
        return -EINVAL;

    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    entry = aesd_circular_buffer_find_entry_offset_for_fpos(
        &dev->buffer, (size_t)*f_pos, &entry_offset);

    if (!entry) {
        mutex_unlock(&dev->lock);
        return 0;
    }

    available = entry->size - entry_offset;
    bytes_to_copy = min(count, available);

    if (copy_to_user(buf, entry->buffptr + entry_offset, bytes_to_copy)) {
        retval = -EFAULT;
        goto out;
    }

    *f_pos += bytes_to_copy;
    retval = bytes_to_copy;

out:
    mutex_unlock(&dev->lock);
    return retval;
}

ssize_t aesd_write(struct file *filp, const char __user *buf, size_t count,
                   loff_t *f_pos)
{
    struct aesd_dev *dev = filp->private_data;
    char *input = NULL;
    char *combined = NULL;
    size_t old_pending_size;
    size_t combined_size;
    size_t command_start;
    size_t command_end;
    size_t command_len;
    size_t consumed;
    size_t newline_pos;
    ssize_t retval;
    struct aesd_buffer_entry entry;

    PDEBUG("write %zu bytes with offset %lld\n", count, *f_pos);

    if (count == 0)
        return 0;

    /*
     * Copy the complete userspace write before modifying driver state.
     * The mutex is held for the complete write operation as required.
     */
    input = kmalloc(count, GFP_KERNEL);
    if (!input)
        return -ENOMEM;

    if (copy_from_user(input, buf, count)) {
        kfree(input);
        return -EFAULT;
    }

    if (mutex_lock_interruptible(&dev->lock)) {
        kfree(input);
        return -ERESTARTSYS;
    }

    old_pending_size = dev->write_buffer_size;
    combined_size = old_pending_size + count;

    combined = kmalloc(combined_size, GFP_KERNEL);
    if (!combined) {
        retval = -ENOMEM;
        goto out_unlock;
    }

    if (old_pending_size)
        memcpy(combined, dev->write_buffer, old_pending_size);

    memcpy(combined + old_pending_size, input, count);

    kfree(dev->write_buffer);
    dev->write_buffer = NULL;
    dev->write_buffer_size = 0;

    /*
     * Process every complete newline-terminated command.
     */
    command_start = 0;

    while (command_start < combined_size) {
        char *newline;

        newline = memchr(combined + command_start, '\n',
                         combined_size - command_start);

        if (!newline)
            break;

        newline_pos = newline - combined;
        command_end = newline_pos + 1;
        command_len = command_end - command_start;

        entry.buffptr = kmalloc(command_len, GFP_KERNEL);
        if (!entry.buffptr) {
            retval = -ENOMEM;
            goto out_cleanup_combined;
        }

        memcpy((char *)entry.buffptr,
               combined + command_start,
               command_len);
        entry.size = command_len;

        /*
         * If the circular buffer is full, in_offs points to the
         * oldest entry which will be overwritten.
         */
        aesd_free_overwritten_entry(&dev->buffer);
        aesd_circular_buffer_add_entry(&dev->buffer, &entry);

        command_start = command_end;
    }

    /*
     * Preserve any unterminated tail for the next write().
     */
    consumed = command_start;

    if (consumed < combined_size) {
        size_t remaining = combined_size - consumed;
        char *remaining_buffer;

        remaining_buffer = kmalloc(remaining, GFP_KERNEL);
        if (!remaining_buffer) {
            /*
             * Keep the already completed commands, but we cannot
             * preserve the incomplete command. Report the allocation
             * failure to the caller.
             */
            retval = -ENOMEM;
            goto out_cleanup_combined;
        }

        memcpy(remaining_buffer, combined + consumed, remaining);

        dev->write_buffer = remaining_buffer;
        dev->write_buffer_size = remaining;
    }

    retval = count;

    goto out_cleanup_combined;

out_cleanup_combined:
    /*
     * Any command allocations already inserted into the circular
     * buffer are owned by the buffer and must not be freed here.
     */
    kfree(combined);

out_unlock:
    mutex_unlock(&dev->lock);
    kfree(input);

    return retval;
}

static loff_t aesd_llseek(struct file *filp, loff_t offset, int whence)
{
    struct aesd_dev *dev = filp->private_data;
    loff_t newpos;
    size_t total_size;

    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    total_size = aesd_buffer_size(&dev->buffer);

    switch (whence) {
    case SEEK_SET:
        newpos = offset;
        break;
    case SEEK_CUR:
        newpos = filp->f_pos + offset;
        break;
    case SEEK_END:
        newpos = total_size + offset;
        break;
    default:
        mutex_unlock(&dev->lock);
        return -EINVAL;
    }

    if (newpos < 0) {
        mutex_unlock(&dev->lock);
        return -EINVAL;
    }

    filp->f_pos = newpos;

    mutex_unlock(&dev->lock);

    return newpos;
}

static long aesd_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    struct aesd_dev *dev = filp->private_data;
    struct aesd_seekto seekto;
    struct aesd_buffer_entry *entry;
    uint8_t entry_index;
    uint8_t entry_count;
    size_t file_pos = 0;
    uint8_t index;

    if (_IOC_TYPE(cmd) != AESD_IOC_MAGIC)
        return -ENOTTY;

    if (_IOC_NR(cmd) > AESDCHAR_IOC_MAXNR)
        return -ENOTTY;

    if (cmd != AESDCHAR_IOCSEEKTO)
        return -ENOTTY;

    if (copy_from_user(&seekto,
                       (struct aesd_seekto __user *)arg,
                       sizeof(seekto)))
        return -EFAULT;

    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    if (dev->buffer.full)
        entry_count = AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
    else
        entry_count = dev->buffer.in_offs;

    if (seekto.write_cmd >= entry_count) {
        mutex_unlock(&dev->lock);
        return -EINVAL;
    }

    entry_index = (dev->buffer.out_offs + seekto.write_cmd) %
                  AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;

    entry = &dev->buffer.entry[entry_index];

    if (seekto.write_cmd_offset >= entry->size) {
        mutex_unlock(&dev->lock);
        return -EINVAL;
    }

    for (index = 0; index < seekto.write_cmd; index++) {
        uint8_t current_index =
            (dev->buffer.out_offs + index) %
            AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;

        file_pos += dev->buffer.entry[current_index].size;
    }

    file_pos += seekto.write_cmd_offset;
    filp->f_pos = file_pos;

    mutex_unlock(&dev->lock);

    return 0;
}

struct file_operations aesd_fops = {
    .owner = THIS_MODULE,
    .read = aesd_read,
    .write = aesd_write,
    .open = aesd_open,
    .release = aesd_release,
    .llseek = aesd_llseek,
    .unlocked_ioctl = aesd_ioctl,
};

static int aesd_setup_cdev(struct aesd_dev *dev)
{
    int err;
    int devno = MKDEV(aesd_major, aesd_minor);

    cdev_init(&dev->cdev, &aesd_fops);
    dev->cdev.owner = THIS_MODULE;
    dev->cdev.ops = &aesd_fops;

    err = cdev_add(&dev->cdev, devno, 1);
    if (err)
        printk(KERN_ERR "Error %d adding aesd cdev\n", err);

    return err;
}

int aesd_init_module(void)
{
    dev_t dev = 0;
    int result;

    result = alloc_chrdev_region(&dev, aesd_minor, 1, "aesdchar");
    if (result < 0) {
        printk(KERN_WARNING "Can't get major %d\n", aesd_major);
        return result;
    }

    aesd_major = MAJOR(dev);

    memset(&aesd_device, 0, sizeof(struct aesd_dev));

    aesd_circular_buffer_init(&aesd_device.buffer);
    mutex_init(&aesd_device.lock);

    result = aesd_setup_cdev(&aesd_device);

    if (result) {
        mutex_destroy(&aesd_device.lock);
        unregister_chrdev_region(dev, 1);
    }

    return result;
}

void aesd_cleanup_module(void)
{
    dev_t devno = MKDEV(aesd_major, aesd_minor);
    struct aesd_buffer_entry *entry;
    uint8_t index;

    cdev_del(&aesd_device.cdev);

    mutex_lock(&aesd_device.lock);

    AESD_CIRCULAR_BUFFER_FOREACH(entry, &aesd_device.buffer, index) {
        if (entry->buffptr) {
            kfree(entry->buffptr);
            entry->buffptr = NULL;
            entry->size = 0;
        }
    }

    kfree(aesd_device.write_buffer);
    aesd_device.write_buffer = NULL;
    aesd_device.write_buffer_size = 0;

    mutex_unlock(&aesd_device.lock);
    mutex_destroy(&aesd_device.lock);

    unregister_chrdev_region(devno, 1);
}

module_init(aesd_init_module);
module_exit(aesd_cleanup_module);
