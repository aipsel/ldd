#include <linux/module.h>
#include <linux/init.h>
#include <linux/moduleparam.h>
#include <linux/kdev_t.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "scull.h"

MODULE_AUTHOR("Aipsel");
MODULE_LICENSE("Dual BSD/GPL");

/* module parameters. */
static int scull_major     = 0;
static int scull_minor     = 0;
static int scull_nr_devs   = 1;
static int scull_quantum   = SCULL_QUANTUM;
static int scull_qset      = SCULL_QSET;

module_param(scull_major, int, S_IRUGO);
module_param(scull_minor, int, S_IRUGO);
module_param(scull_nr_devs, int, S_IRUGO);
module_param(scull_quantum, int, S_IRUGO);
module_param(scull_qset, int, S_IRUGO);

static struct scull_dev *devices;

/* All syscalls. */
static int scull_open(struct inode *inode, struct file *filp);
static int scull_release(struct inode *inode, struct file *filp);
static ssize_t scull_read(struct file *filp, char __user *buff,
    size_t count, loff_t *offp);
static ssize_t scull_write(struct file *filp, const char __user *buff,
    size_t count, loff_t *offp);

static struct file_operations scull_fops = {
    .owner       = THIS_MODULE,
    .open        = scull_open,
    .release     = scull_release,
    .read        = scull_read,
    .write       = scull_write,
};

static void scull_trim(struct scull_dev *dev) {
    struct scull_qset *qset;
    void **q;

    for (qset = dev->data; qset; qset = dev->data) {
        dev->data = qset->next;
        if ((q = qset->data) /* != NULL */) {
            while (*q)
                kfree(*q++);
            kfree(qset->data);
        }
        kfree(qset);
    }
    // dev->data is already NULL.
    dev->size = 0;
}

static int scull_open(struct inode *inode, struct file *filp) {
    struct scull_dev *dev;

    dev = container_of(inode->i_cdev, struct scull_dev, cdev);
    filp->private_data = dev;
    if ((filp->f_flags & O_ACCMODE) == O_WRONLY)
        scull_trim(dev);
    return 0;
}

static int scull_release(struct inode *inode, struct file *filp) {
    return 0; // do nothing.
}

static ssize_t scull_read(struct file *filp, char __user *buff,
    size_t count, loff_t *offp)
{
    struct scull_dev *dev = filp->private_data;
    struct scull_qset *qset = dev->data;
    int p_pos = *offp / dev->quantum, s_pos = *offp % dev->quantum;
    ssize_t rv = 0;

    if (down_interruptible(&dev->sem))
        return -ERESTARTSYS;
    if (*offp >= dev->size)
        goto out;

    while ((p_pos >= dev->qset) && qset) {
        p_pos -= dev->qset;
        qset = qset->next;
    }
    if (!qset || !qset->data || !qset->data[p_pos])
        goto out;
    if (*offp + count > dev->size)
        count = dev->size - *offp;
    if (count > dev->quantum - s_pos)
        count = dev->quantum - s_pos;
    pr_alert("scull read: %d, %d, count: %d.\n", p_pos, s_pos, count);
    if (copy_to_user(buff, qset->data[p_pos] + s_pos, count)) {
        rv = -EFAULT;
        goto out;
    }
    *offp += count;
    rv = count;

out:
    up(&dev->sem);
    return rv;
}

static ssize_t scull_write(struct file *filp, const char __user *buff,
    size_t count, loff_t *offp)
{
    struct scull_dev *dev = filp->private_data;
    struct scull_qset *qset = dev->data;
    int p_pos = *offp / dev->quantum, s_pos = *offp % dev->quantum;
    ssize_t rv = -ENOMEM;

    if (down_interruptible(&dev->sem))
        return -ERESTARTSYS;
    /* allocate first qset explicitly if need be. */
    if (!qset) {
        if (!(dev->data = kmalloc(sizeof(struct scull_qset), GFP_KERNEL)))
            goto out;
        memset(dev->data, 0, sizeof(struct scull_qset));
        qset = dev->data;
    }
    while (p_pos >= dev->qset) {
        p_pos -= dev->qset;
        if (!qset->next) {
            if (!(qset->next = kmalloc(sizeof(struct scull_qset), GFP_KERNEL)))
                goto out;
            memset(qset->next, 0, sizeof(struct scull_qset));
        }
        qset = qset->next;
    }
    if (!qset->data) {
        if (!(qset->data = kmalloc(dev->qset * sizeof(char *), GFP_KERNEL)))
            goto out;
        memset(qset->data, 0, dev->qset * sizeof(char *));
    }
    if (!qset->data[p_pos]) {
        if (!(qset->data[p_pos] = kmalloc(dev->quantum, GFP_KERNEL)))
            goto out;
    }
    if (count > dev->quantum - s_pos)
        count = dev->quantum - s_pos;
    pr_alert("scull write: %d, %d, count: %d.\n", p_pos, s_pos, count);
    if (copy_from_user(qset->data[p_pos] + s_pos, buff, count)) {
        rv = -EFAULT;
        goto out;
    }
    *offp += count;
    rv = count;
    if (dev->size < *offp)
        dev->size = *offp;

out:
    up(&dev->sem);
    return rv;
}

static void __init scull_setup_cdev(struct scull_dev *dev, int index) {
    dev_t devno = MKDEV(scull_major, scull_minor + index);
    int err;

    cdev_init(&dev->cdev, &scull_fops);
    dev->cdev.owner = THIS_MODULE;
    err = cdev_add(&dev->cdev, devno, 1);
    if (err)
        pr_alert("can\'t add scull%d, errno: %d.\n", index, err);
}

static int __init scull_init(void) {
    dev_t dev;
    int result, i;

    /* register device number. */
    if (scull_major) {
        dev = MKDEV(scull_major, scull_minor);
        result = register_chrdev_region(dev, scull_nr_devs, "scull");
    } else {
        result = alloc_chrdev_region(&dev, scull_minor, scull_nr_devs, "scull");
        scull_major = MAJOR(dev);
    }
    if (result < 0) {
        pr_alert("can't allocate %d.\n", scull_major);
        return result;
    }

    /* allocate scull devices. */
    devices = kmalloc(scull_nr_devs * sizeof(struct scull_dev), GFP_KERNEL);
    if (!devices) {
        pr_alert("can\'t allocate memary for devices.\n");
        unregister_chrdev_region(dev, scull_nr_devs);
        return -ENOMEM;
    }
    memset(devices, 0, scull_nr_devs * sizeof(struct scull_dev));
    for (i = 0; i < scull_nr_devs; i++) {
        devices[i].quantum = scull_quantum;
        devices[i].qset = scull_qset;
        init_MUTEX(&devices[i].sem);
        scull_setup_cdev(devices + i, i);
    }
    return 0;
}

static void __exit scull_exit(void) {
    int i;
    if (devices) {
        for (i = 0; i < scull_nr_devs; i++) {
            scull_trim(devices + i);
            cdev_del(&devices[i].cdev);
        }
        kfree(devices);
    }
    unregister_chrdev_region(MKDEV(scull_major, scull_minor), scull_nr_devs);
}

module_init(scull_init);
module_exit(scull_exit);
