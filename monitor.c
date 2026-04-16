/* monitor.c — Linux Kernel Module: Container Memory Monitor
 * OS-Jackfruit Project
 *
 * Build:   make (uses the Makefile's kernel target)
 * Load:    sudo insmod monitor.ko
 * Verify:  ls -l /dev/container_monitor && dmesg | tail
 * Unload:  sudo rmmod monitor
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/slab.h>          /* kmalloc / kfree              */
#include <linux/workqueue.h>     /* delayed_work                 */
#include <linux/pid.h>           /* find_vpid / pid_task         */
#include <linux/sched.h>         /* task_struct                  */
#include <linux/sched/signal.h>  /* send_sig                     */
#include <linux/mm.h>            /* get_mm_rss                   */
#include <linux/uaccess.h>       /* copy_from_user               */
#include <linux/version.h>       /* LINUX_VERSION_CODE           */

#include "monitor_ioctl.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("OS-Jackfruit");
MODULE_DESCRIPTION("Per-container RSS monitor with soft/hard limits");
MODULE_VERSION("0.1");

/* ═══════════════════════════════════════════════════════════════════════════
 *  DEVICE SETUP
 * ═══════════════════════════════════════════════════════════════════════════ */
#define DEVICE_NAME "container_monitor"
#define CLASS_NAME  "jackfruit"

static int            major_num;
static struct class  *monitor_class;
static struct device *monitor_device;

/* ═══════════════════════════════════════════════════════════════════════════
 *  TRACKED CONTAINER LIST
 * ═══════════════════════════════════════════════════════════════════════════ */
struct container_entry {
    struct list_head list;
    pid_t            pid;
    long             soft_limit_kb;
    long             hard_limit_kb;
    int              soft_warned;   /* 1 after first soft warning */
};

static LIST_HEAD(container_list);
static DEFINE_MUTEX(list_mutex);

/* ═══════════════════════════════════════════════════════════════════════════
 *  RSS HELPER
 *
 *  Returns Resident Set Size in KB for the given pid.
 *  Returns -1 if the task no longer exists.
 * ═══════════════════════════════════════════════════════════════════════════ */
static long get_rss_kb(pid_t pid)
{
    struct task_struct *task;
    long rss = -1;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (task && task->mm) {
        rss = (long)get_mm_rss(task->mm) * (PAGE_SIZE / 1024L);
    }
    rcu_read_unlock();
    return rss;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  PERIODIC MEMORY CHECK  (Task 4)
 * ═══════════════════════════════════════════════════════════════════════════ */
static struct delayed_work monitor_work;
#define CHECK_INTERVAL_MS 1000

static void monitor_check(struct work_struct *work)
{
    struct container_entry *entry, *tmp;

    mutex_lock(&list_mutex);

    list_for_each_entry_safe(entry, tmp, &container_list, list) {
        long rss = get_rss_kb(entry->pid);

        /* ── Process gone → remove stale entry ─────────────────────────── */
        if (rss < 0) {
            pr_info("jackfruit: pid %d no longer exists, removing\n",
                    entry->pid);
            list_del(&entry->list);
            kfree(entry);
            continue;
        }

        /* ── Hard limit ─────────────────────────────────────────────────── */
        if (entry->hard_limit_kb > 0 && rss > entry->hard_limit_kb) {
            pr_warn("jackfruit: pid %d hard limit exceeded "
                    "(%ld KB used > %ld KB limit) — killing\n",
                    entry->pid, rss, entry->hard_limit_kb);

            /* Send SIGKILL to the container process */
            rcu_read_lock();
            struct task_struct *t = pid_task(find_vpid(entry->pid), PIDTYPE_PID);
            if (t) send_sig(SIGKILL, t, 1);
            rcu_read_unlock();

            list_del(&entry->list);
            kfree(entry);
            continue;
        }

        /* ── Soft limit (warn once) ──────────────────────────────────────── */
        if (entry->soft_limit_kb > 0 &&
            rss > entry->soft_limit_kb &&
            !entry->soft_warned) {

            pr_warn("jackfruit: pid %d soft limit exceeded "
                    "(%ld KB used > %ld KB limit) — warning\n",
                    entry->pid, rss, entry->soft_limit_kb);
            entry->soft_warned = 1;
        }
    }

    mutex_unlock(&list_mutex);

    /* Reschedule for the next check */
    schedule_delayed_work(&monitor_work,
                          msecs_to_jiffies(CHECK_INTERVAL_MS));
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  IOCTL HANDLER
 * ═══════════════════════════════════════════════════════════════════════════ */
static long monitor_ioctl(struct file *file,
                          unsigned int  cmd,
                          unsigned long arg)
{
    (void)file;

    switch (cmd) {

    /* ── Register a container PID ──────────────────────────────────────── */
    case MONITOR_REGISTER: {
        struct container_limits lim;
        if (copy_from_user(&lim, (void __user *)arg, sizeof(lim)))
            return -EFAULT;

        struct container_entry *entry = kmalloc(sizeof(*entry), GFP_KERNEL);
        if (!entry) return -ENOMEM;

        entry->pid           = lim.pid;
        entry->soft_limit_kb = lim.soft_limit_kb;
        entry->hard_limit_kb = lim.hard_limit_kb;
        entry->soft_warned   = 0;
        INIT_LIST_HEAD(&entry->list);

        mutex_lock(&list_mutex);
        list_add_tail(&entry->list, &container_list);
        mutex_unlock(&list_mutex);

        pr_info("jackfruit: registered pid=%d soft=%ldKB hard=%ldKB\n",
                lim.pid, lim.soft_limit_kb, lim.hard_limit_kb);
        return 0;
    }

    /* ── Unregister a container PID ────────────────────────────────────── */
    case MONITOR_UNREGISTER: {
        pid_t pid;
        if (copy_from_user(&pid, (void __user *)arg, sizeof(pid)))
            return -EFAULT;

        mutex_lock(&list_mutex);
        struct container_entry *entry, *tmp;
        list_for_each_entry_safe(entry, tmp, &container_list, list) {
            if (entry->pid == pid) {
                list_del(&entry->list);
                kfree(entry);
                pr_info("jackfruit: unregistered pid=%d\n", pid);
                break;
            }
        }
        mutex_unlock(&list_mutex);
        return 0;
    }

    default:
        return -ENOTTY;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  FILE OPERATIONS
 * ═══════════════════════════════════════════════════════════════════════════ */
static const struct file_operations monitor_fops = {
    .owner          = THIS_MODULE,
    .unlocked_ioctl = monitor_ioctl,
};

/* ═══════════════════════════════════════════════════════════════════════════
 *  MODULE INIT / EXIT
 * ═══════════════════════════════════════════════════════════════════════════ */
static int __init monitor_init(void)
{
    major_num = register_chrdev(0, DEVICE_NAME, &monitor_fops);
    if (major_num < 0) {
        pr_err("jackfruit: register_chrdev failed: %d\n", major_num);
        return major_num;
    }

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
    monitor_class = class_create(CLASS_NAME);
#else
    monitor_class = class_create(THIS_MODULE, CLASS_NAME);
#endif
    if (IS_ERR(monitor_class)) {
        unregister_chrdev(major_num, DEVICE_NAME);
        return PTR_ERR(monitor_class);
    }

    monitor_device = device_create(monitor_class, NULL,
                                   MKDEV(major_num, 0),
                                   NULL, DEVICE_NAME);
    if (IS_ERR(monitor_device)) {
        class_destroy(monitor_class);
        unregister_chrdev(major_num, DEVICE_NAME);
        return PTR_ERR(monitor_device);
    }

    INIT_DELAYED_WORK(&monitor_work, monitor_check);
    schedule_delayed_work(&monitor_work,
                          msecs_to_jiffies(CHECK_INTERVAL_MS));

    pr_info("jackfruit: monitor loaded (major=%d, check_interval=%dms)\n",
            major_num, CHECK_INTERVAL_MS);
    return 0;
}

static void __exit monitor_exit(void)
{
    cancel_delayed_work_sync(&monitor_work);

    mutex_lock(&list_mutex);
    struct container_entry *entry, *tmp;
    list_for_each_entry_safe(entry, tmp, &container_list, list) {
        list_del(&entry->list);
        kfree(entry);
    }
    mutex_unlock(&list_mutex);

    device_destroy(monitor_class, MKDEV(major_num, 0));
    class_destroy(monitor_class);
    unregister_chrdev(major_num, DEVICE_NAME);

    pr_info("jackfruit: monitor unloaded\n");
}

module_init(monitor_init);
module_exit(monitor_exit);
