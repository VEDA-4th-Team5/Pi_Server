// SPDX-License-Identifier: GPL-2.0
/*
 * parking_alert.c - 스마트 주차 알림 상태를 제공하는 Linux 문자 디바이스.
 *
 * /dev/parking_alert는 32개 슬롯의 활성 알림을 비트 마스크로 보관한다.
 * read/poll은 상태 변경 통지를, write/ioctl은 상태 제어를 제공한다.
 */
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#include "parking_alert.h"

#define PARKING_ALERT_DEVICE_NAME "parking_alert"

struct parking_alert_device {
    dev_t devt;
    struct cdev cdev;
    struct class *class;
    struct device *device;
    struct mutex lock;
    wait_queue_head_t wait_queue;
    __u32 active_mask;
    __u32 last_operation;
    __u32 last_slot;
    __u64 generation;
    __u64 last_event_id;
};

struct parking_alert_file_context {
    __u64 observed_generation;
    bool initial_read_pending;
};

static struct parking_alert_device parking_alert;

static void parking_alert_fill_state_locked(struct parking_alert_state *state)
{
    memset(state, 0, sizeof(*state));
    state->version = PARKING_ALERT_API_VERSION;
    state->active_mask = parking_alert.active_mask;
    state->last_operation = parking_alert.last_operation;
    state->last_slot = parking_alert.last_slot;
    state->generation = parking_alert.generation;
    state->last_event_id = parking_alert.last_event_id;
}

static int parking_alert_validate_command(const struct parking_alert_command *command)
{
    if (command->version != PARKING_ALERT_API_VERSION)
        return -EINVAL;

    switch (command->operation) {
    case PARKING_ALERT_OP_SET_SLOT:
    case PARKING_ALERT_OP_CLEAR_SLOT:
        return command->slot_index < PARKING_ALERT_MAX_SLOTS ? 0 : -EINVAL;
    case PARKING_ALERT_OP_CLEAR_ALL:
        return 0;
    default:
        return -EINVAL;
    }
}

static int parking_alert_apply_command(const struct parking_alert_command *command)
{
    __u32 previous_mask;
    int ret;

    ret = parking_alert_validate_command(command);
    if (ret)
        return ret;

    mutex_lock(&parking_alert.lock);
    previous_mask = parking_alert.active_mask;

    switch (command->operation) {
    case PARKING_ALERT_OP_SET_SLOT:
        parking_alert.active_mask |= BIT(command->slot_index);
        break;
    case PARKING_ALERT_OP_CLEAR_SLOT:
        parking_alert.active_mask &= ~BIT(command->slot_index);
        break;
    case PARKING_ALERT_OP_CLEAR_ALL:
        parking_alert.active_mask = 0;
        break;
    default:
        mutex_unlock(&parking_alert.lock);
        return -EINVAL;
    }

    /* 같은 상태를 반복 설정하면 새 이벤트로 알리지 않는다. */
    if (previous_mask != parking_alert.active_mask) {
        parking_alert.last_operation = command->operation;
        parking_alert.last_slot = command->operation == PARKING_ALERT_OP_CLEAR_ALL
                                      ? PARKING_ALERT_NO_SLOT
                                      : command->slot_index;
        parking_alert.last_event_id = command->event_id;
        ++parking_alert.generation;
        mutex_unlock(&parking_alert.lock);
        wake_up_interruptible(&parking_alert.wait_queue);
        return 0;
    }

    mutex_unlock(&parking_alert.lock);
    return 0;
}

static int parking_alert_open(struct inode *inode, struct file *file)
{
    struct parking_alert_file_context *context;

    context = kzalloc(sizeof(*context), GFP_KERNEL);
    if (!context)
        return -ENOMEM;

    context->initial_read_pending = true;
    file->private_data = context;
    return nonseekable_open(inode, file);
}

static int parking_alert_release(struct inode *inode, struct file *file)
{
    (void)inode;
    kfree(file->private_data);
    file->private_data = NULL;
    return 0;
}

static ssize_t parking_alert_read(struct file *file, char __user *buffer,
                                  size_t count, loff_t *offset)
{
    struct parking_alert_file_context *context = file->private_data;
    struct parking_alert_state state;
    int ret;

    (void)offset;
    if (count < sizeof(state))
        return -EINVAL;

    if (!context->initial_read_pending) {
        if (file->f_flags & O_NONBLOCK) {
            if (READ_ONCE(parking_alert.generation) == context->observed_generation)
                return -EAGAIN;
        } else {
            ret = wait_event_interruptible(
                parking_alert.wait_queue,
                READ_ONCE(parking_alert.generation) != context->observed_generation);
            if (ret)
                return ret;
        }
    }

    mutex_lock(&parking_alert.lock);
    parking_alert_fill_state_locked(&state);
    context->observed_generation = state.generation;
    context->initial_read_pending = false;
    mutex_unlock(&parking_alert.lock);

    if (copy_to_user(buffer, &state, sizeof(state)))
        return -EFAULT;
    return sizeof(state);
}

static ssize_t parking_alert_write(struct file *file, const char __user *buffer,
                                   size_t count, loff_t *offset)
{
    struct parking_alert_command command;
    int ret;

    (void)file;
    (void)offset;
    if (count != sizeof(command))
        return -EINVAL;
    if (copy_from_user(&command, buffer, sizeof(command)))
        return -EFAULT;

    ret = parking_alert_apply_command(&command);
    return ret ? ret : (ssize_t)count;
}

static __poll_t parking_alert_poll(struct file *file, poll_table *wait)
{
    struct parking_alert_file_context *context = file->private_data;
    __poll_t mask = 0;

    poll_wait(file, &parking_alert.wait_queue, wait);
    if (context->initial_read_pending ||
        READ_ONCE(parking_alert.generation) != context->observed_generation)
        mask |= EPOLLIN | EPOLLRDNORM;
    return mask;
}

static long parking_alert_ioctl(struct file *file, unsigned int command,
                                unsigned long argument)
{
    struct parking_alert_command alert_command;
    struct parking_alert_state state;

    (void)file;
    switch (command) {
    case PARKING_ALERT_IOC_SET_SLOT:
    case PARKING_ALERT_IOC_CLEAR_SLOT:
        if (copy_from_user(&alert_command, (void __user *)argument,
                           sizeof(alert_command)))
            return -EFAULT;
        alert_command.operation = command == PARKING_ALERT_IOC_SET_SLOT
                                      ? PARKING_ALERT_OP_SET_SLOT
                                      : PARKING_ALERT_OP_CLEAR_SLOT;
        return parking_alert_apply_command(&alert_command);

    case PARKING_ALERT_IOC_CLEAR_ALL:
        memset(&alert_command, 0, sizeof(alert_command));
        alert_command.version = PARKING_ALERT_API_VERSION;
        alert_command.operation = PARKING_ALERT_OP_CLEAR_ALL;
        alert_command.slot_index = PARKING_ALERT_NO_SLOT;
        return parking_alert_apply_command(&alert_command);

    case PARKING_ALERT_IOC_GET_STATE:
        mutex_lock(&parking_alert.lock);
        parking_alert_fill_state_locked(&state);
        mutex_unlock(&parking_alert.lock);
        return copy_to_user((void __user *)argument, &state, sizeof(state))
                   ? -EFAULT
                   : 0;
    default:
        return -ENOTTY;
    }
}

static const struct file_operations parking_alert_fops = {
    .owner = THIS_MODULE,
    .open = parking_alert_open,
    .release = parking_alert_release,
    .read = parking_alert_read,
    .write = parking_alert_write,
    .poll = parking_alert_poll,
    .unlocked_ioctl = parking_alert_ioctl,
#ifdef CONFIG_COMPAT
    .compat_ioctl = parking_alert_ioctl,
#endif
};

static int __init parking_alert_init(void)
{
    int ret;

    mutex_init(&parking_alert.lock);
    init_waitqueue_head(&parking_alert.wait_queue);
    parking_alert.last_slot = PARKING_ALERT_NO_SLOT;

    ret = alloc_chrdev_region(&parking_alert.devt, 0, 1,
                              PARKING_ALERT_DEVICE_NAME);
    if (ret)
        return ret;

    cdev_init(&parking_alert.cdev, &parking_alert_fops);
    parking_alert.cdev.owner = THIS_MODULE;
    ret = cdev_add(&parking_alert.cdev, parking_alert.devt, 1);
    if (ret)
        goto unregister_region;

    parking_alert.class = class_create(PARKING_ALERT_DEVICE_NAME);
    if (IS_ERR(parking_alert.class)) {
        ret = PTR_ERR(parking_alert.class);
        goto delete_cdev;
    }

    parking_alert.device = device_create(
        parking_alert.class, NULL, parking_alert.devt, NULL,
        PARKING_ALERT_DEVICE_NAME);
    if (IS_ERR(parking_alert.device)) {
        ret = PTR_ERR(parking_alert.device);
        goto destroy_class;
    }

    pr_info("parking_alert: registered /dev/%s (major=%u minor=%u)\n",
            PARKING_ALERT_DEVICE_NAME, MAJOR(parking_alert.devt),
            MINOR(parking_alert.devt));
    return 0;

destroy_class:
    class_destroy(parking_alert.class);
delete_cdev:
    cdev_del(&parking_alert.cdev);
unregister_region:
    unregister_chrdev_region(parking_alert.devt, 1);
    return ret;
}

static void __exit parking_alert_exit(void)
{
    device_destroy(parking_alert.class, parking_alert.devt);
    class_destroy(parking_alert.class);
    cdev_del(&parking_alert.cdev);
    unregister_chrdev_region(parking_alert.devt, 1);
    pr_info("parking_alert: unregistered\n");
}

module_init(parking_alert_init);
module_exit(parking_alert_exit);

MODULE_AUTHOR("VEDA Team 5");
MODULE_DESCRIPTION("Smart parking alert character device");
MODULE_LICENSE("GPL");
MODULE_VERSION("1.0.0");
