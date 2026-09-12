/**
 * @file aesdchar.c
 * @brief Functions and data related to the AESD char driver implementation
 *
 * Based on the implementation of the "scull" device driver, found in
 * Linux Device Drivers example code.
 *
 * @author Dan Walkes
 * @date 2019-10-22
 * @copyright Copyright (c) 2019
 *
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/fs.h> // file_operations
#include "aesdchar.h"

#include <linux/slab.h>
#include <linux/uaccess.h> 
#include <linux/string.h>  
int aesd_major =   0; // use dynamic major
int aesd_minor =   0;

MODULE_AUTHOR("Reza Setiabekti"); /** TODO: fill in your name **/
MODULE_LICENSE("Dual BSD/GPL");

struct aesd_dev aesd_device;

int aesd_open(struct inode *inode, struct file *filp)
{
    struct aesd_dev *dev;
    PDEBUG("open");
    /**
     * TODO: handle open
     */
    dev = container_of(inode->i_cdev, struct aesd_dev, cdev);
    filp->private_data = dev;
    return 0;
}

int aesd_release(struct inode *inode, struct file *filp)
{
    PDEBUG("release");
    /**
     * TODO: handle release
     */
    return 0;
}

ssize_t aesd_read(struct file *filp, char __user *buf, size_t count,
                loff_t *f_pos)
{
    ssize_t retval = 0;
    struct aesd_dev *dev = filp->private_data;
    struct aesd_buffer_entry *entry;
    size_t entry_offset = 0;
    size_t bytes_available;
    size_t bytes_to_copy;

    PDEBUG("read %zu bytes with offset %lld",count,*f_pos);

    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    /* find the entry containing the byte at *f_pos */
    entry = aesd_circular_buffer_find_entry_offset_for_fpos(&dev->buffer,
                                                            *f_pos, &entry_offset);
    if (!entry) {
        /* nothing at this position -> EOF */
        retval = 0;
        goto out;
    }

    /* only return up to the end of THIS entry in a single read */
    bytes_available = entry->size - entry_offset;
    bytes_to_copy = (count < bytes_available) ? count : bytes_available;

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
    ssize_t retval = -ENOMEM;
    struct aesd_dev *dev = filp->private_data;
    char *new_buf;
    const char *old_entry_to_free = NULL;

    PDEBUG("write %zu bytes with offset %lld",count,*f_pos);

    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    /* grow the working entry to hold the new bytes */
    new_buf = krealloc(dev->working_entry.buffptr,
                       dev->working_entry.size + count, GFP_KERNEL);
    if (!new_buf) {
        mutex_unlock(&dev->lock);
        return -ENOMEM;
    }
    dev->working_entry.buffptr = new_buf;

    /* copy the incoming bytes from userspace onto the end */
    if (copy_from_user((char *)dev->working_entry.buffptr + dev->working_entry.size,
                       buf, count)) {
        mutex_unlock(&dev->lock);
        return -EFAULT;
    }
    dev->working_entry.size += count;
    retval = count;

    /* if a newline is present, the command is complete */
    if (memchr(dev->working_entry.buffptr, '\n', dev->working_entry.size)) {
        /* if buffer is full, the entry about to be overwritten must be freed */
        if (dev->buffer.full) {
            old_entry_to_free = dev->buffer.entry[dev->buffer.in_offs].buffptr;
        }
        aesd_circular_buffer_add_entry(&dev->buffer, &dev->working_entry);
        /* reset working entry for the next command */
        dev->working_entry.buffptr = NULL;
        dev->working_entry.size = 0;
    }

    mutex_unlock(&dev->lock);

    if (old_entry_to_free)
        kfree(old_entry_to_free);

    return retval;
}
struct file_operations aesd_fops = {
    .owner =    THIS_MODULE,
    .read =     aesd_read,
    .write =    aesd_write,
    .open =     aesd_open,
    .release =  aesd_release,
};

static int aesd_setup_cdev(struct aesd_dev *dev)
{
    int err, devno = MKDEV(aesd_major, aesd_minor);

    cdev_init(&dev->cdev, &aesd_fops);
    dev->cdev.owner = THIS_MODULE;
    dev->cdev.ops = &aesd_fops;
    err = cdev_add (&dev->cdev, devno, 1);
    if (err) {
        printk(KERN_ERR "Error %d adding aesd cdev", err);
    }
    return err;
}



int aesd_init_module(void)
{
    dev_t dev = 0;
    int result;
    result = alloc_chrdev_region(&dev, aesd_minor, 1,
            "aesdchar");
    aesd_major = MAJOR(dev);
    if (result < 0) {
        printk(KERN_WARNING "Can't get major %d\n", aesd_major);
        return result;
    }
    memset(&aesd_device,0,sizeof(struct aesd_dev));

    /**
     * TODO: initialize the AESD specific portion of the device
     */
    mutex_init(&aesd_device.lock);
    aesd_circular_buffer_init(&aesd_device.buffer);

    result = aesd_setup_cdev(&aesd_device);

    if( result ) {
        unregister_chrdev_region(dev, 1);
    }
    return result;

}

void aesd_cleanup_module(void)
{
    dev_t devno = MKDEV(aesd_major, aesd_minor);
    uint8_t index;
    struct aesd_buffer_entry *entry;

    cdev_del(&aesd_device.cdev);

    AESD_CIRCULAR_BUFFER_FOREACH(entry, &aesd_device.buffer, index) {
        if (entry->buffptr) {
            kfree(entry->buffptr);
            entry->buffptr = NULL;
        }
    }
    if (aesd_device.working_entry.buffptr) {
        kfree(aesd_device.working_entry.buffptr);
    }
    mutex_destroy(&aesd_device.lock);

    unregister_chrdev_region(devno, 1);
}



module_init(aesd_init_module);
module_exit(aesd_cleanup_module);
