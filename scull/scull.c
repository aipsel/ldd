#include <linux/module.h>
#include <linux/init.h>
#include <linux/moduleparam.h>
#include <linux/kdev_t.h>
#include <linux/fs.h>

MODULE_AUTHOR("Aipsel");
MODULE_LICENSE("Dual BSD/GPL");

/* module parameters. */
static int scull_major     = 0;
static int scull_minor     = 0;
static int scull_nr_devs   = 1;

module_param(scull_major, int, S_IRUGO);
module_param(scull_minor, int, S_IRUGO);
module_param(scull_nr_devs, int, S_IRUGO);

static int result; // 0 if registration is success.

static int __init scull_init(void) {
    dev_t dev;

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
    return 0;
}

static void __exit scull_exit(void) {
    if (result == 0) {
        unregister_chrdev_region(MKDEV(scull_major, scull_minor),
            scull_nr_devs);
    }
}

module_init(scull_init);
module_exit(scull_exit);

