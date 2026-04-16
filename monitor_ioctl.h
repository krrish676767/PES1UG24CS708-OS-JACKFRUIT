/* monitor_ioctl.h — shared ioctl definitions between user space and kernel
 * OS-Jackfruit Project
 * Include in both engine.c (user) and monitor.c (kernel). */

#ifndef MONITOR_IOCTL_H
#define MONITOR_IOCTL_H

#ifdef __KERNEL__
#  include <linux/ioctl.h>
#  include <linux/types.h>
#else
#  include <sys/ioctl.h>
#  include <sys/types.h>
#endif

#define MONITOR_MAGIC 'J'   /* 'J' for Jackfruit */

/* Passed to MONITOR_REGISTER */
struct container_limits {
    pid_t pid;
    long  soft_limit_kb;   /* warn when RSS exceeds this */
    long  hard_limit_kb;   /* kill when RSS exceeds this */
};

#define MONITOR_REGISTER   _IOW(MONITOR_MAGIC, 1, struct container_limits)
#define MONITOR_UNREGISTER _IOW(MONITOR_MAGIC, 2, pid_t)

#endif /* MONITOR_IOCTL_H */
