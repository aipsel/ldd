#ifndef _SCULL_H
#define _SCULL_H

#include <linux/cdev.h>
#include <linux/semaphore.h>

#define SCULL_QUANTUM   4000
#define SCULL_QSET      1000

struct scull_qset {
    void **data;
    struct scull_qset *next;
};

struct scull_dev {
    struct scull_qset *data;
    int quantum;
    int qset;
    unsigned long size;
    struct semaphore sem;
    struct cdev cdev;
};

#endif /* _SCULL_H */
