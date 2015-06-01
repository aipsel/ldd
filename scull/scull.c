#include <linux/module.h>
#include <linux/init.h>
#include <linux/moduleparam.h>
#include <linux/kdev_t.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>

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
    int i;

    for (qset = dev->data; qset; qset = dev->data) {
        dev->data = qset->next;
        if (qset->data) {
            for (i = 0; i < dev->qset && qset->data[i]; i++)
                kfree(qset->data[i]);
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

static int scull_read_proc(char *page, char **start, off_t offset, int count,
    int *eof, void *data)
{
    struct scull_qset *qset;
    int n_qset, n_q, i, len = 0;

    for (i = 0; i < scull_nr_devs; i++) {
        if (down_interruptible(&devices[i].sem))
            return -ERESTARTSYS;
        len += sprintf(page + len, "Device %d:\n", i);
        for (qset = devices[i].data, n_qset = 0; qset;
             qset = qset->next, n_qset++)
        {
            if (qset->data) {
                for (n_q = 0; n_q < devices[i].qset && qset->data[n_q]; n_q++)
                    ;
                len += sprintf(page + len, "  qset%d:\t%d\n", n_qset, n_q);
            }
        }
        up(&devices[i].sem);
    }
    *eof = 1;
    return len;
}

/* use seq_file. */
static void *scull_seq_start(struct seq_file *sfile, loff_t *pos) {
    return *pos >= scull_nr_devs ? NULL : devices + *pos;
}

static void *scull_seq_next(struct seq_file *sfile, void *data, loff_t *pos) {
    (*pos)++;
    return *pos >= scull_nr_devs ? NULL : devices + *pos;
}

static int scull_seq_show(struct seq_file *sfile, void *data) {
    struct scull_dev *dev = data;
    struct scull_qset *qset;
    int i;
    if (down_interruptible(&dev->sem))
        return -ERESTARTSYS;
    seq_printf(sfile, "device %i: qset %i, q %i, sz %li\n",
        (int)(dev - devices), dev->qset, dev->quantum, dev->size);
    for (qset = dev->data; qset; qset = qset->next) {
        seq_printf(sfile, "  item at %p, qset at %p\n", qset, qset->data);
        if (qset->data /* && !qset->next */ ) {
            for (i = 0; i < dev->qset && qset->data[i]; i++) {
                seq_printf(sfile, "    %i\t: %p\n", i, qset->data[i]);
            }
        }
    }
    up(&dev->sem);
    return 0;
}

static void scull_seq_stop(struct seq_file *sfile, void *data) {
}

static struct seq_operations scull_seq_ops = {
    .start  = scull_seq_start,
    .next   = scull_seq_next,
    .show   = scull_seq_show,
    .stop   = scull_seq_stop
};

static int scull_proc_open(struct inode *inode, struct file *filp) {
    return seq_open(filp, &scull_seq_ops);
}

static struct file_operations scull_proc_fops = {
    .owner       = THIS_MODULE,
    .open        = scull_proc_open,
    .read        = seq_read,
    .llseek      = seq_lseek,
    .release     = seq_release,
};

static void __init scull_setup_seq_proc(void) {
    struct proc_dir_entry *proc_entry;
    proc_entry = create_proc_entry("scullseq", 0, NULL);
    if (proc_entry)
        proc_entry->proc_fops = &scull_proc_fops;
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
    create_proc_read_entry("scullmem", 0, NULL, scull_read_proc, NULL);
    scull_setup_seq_proc();
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
    remove_proc_entry("scullmem", NULL);
    remove_proc_entry("scullseq", NULL);
}

module_init(scull_init);
module_exit(scull_exit);

