#include <linux/module.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/printk.h>
#include <linux/of.h>
#include <linux/io.h>
#include <linux/reboot.h>

#define DEVICE_NAME "last_kmsg"
#define LAST_KMSG_SIZE (2 * 1024 * 1024)
#define MARKER_VALID 0xDEADBEEF
#define MARKER_OFFSET 0  /* store marker at beginning */

static char *last_kmsg_buf;
static size_t last_kmsg_pos;
static dev_t devno;
static struct cdev last_kmsg_cdev;

/* pointer to the marker at start of buffer */
static u32 *marker;

static ssize_t last_kmsg_read(struct file *f, char __user *buf,
                              size_t len, loff_t *off)
{
    if (*marker != MARKER_VALID)
    {
        const char *na = "NA\n";
        if (*off >= 3)  /* length of "NA\n" */
            return 0;
        if (copy_to_user(buf, na + *off, 3 - *off))
            return -EFAULT;
        *off = 3;
        return 3;
    }

    if (*off >= last_kmsg_pos)
        return 0;

    if (len > last_kmsg_pos - *off)
        len = last_kmsg_pos - *off;

    if (copy_to_user(buf, last_kmsg_buf + *off, len))
        return -EFAULT;

    *off += len;
    return len;
}

static const struct file_operations last_kmsg_fops = {
    .owner = THIS_MODULE,
    .read = last_kmsg_read,
};

/* console hook to store printk output */
static void last_kmsg_console_write(struct console *con,
                                    const char *text, unsigned int len)
{
    /* don't log if buffer full */
    if (!last_kmsg_buf)
        return;

    if (last_kmsg_pos + len >= LAST_KMSG_SIZE)
        return;

    memcpy(last_kmsg_buf + last_kmsg_pos, text, len);
    last_kmsg_pos += len;
}

/* simple reboot notifier to mark buffer valid at crash */
static int last_kmsg_reboot(struct notifier_block *nb,
                            unsigned long event, void *ptr)
{
    /* mark buffer valid on reboot */
    *marker = MARKER_VALID;

    /* optional: reset current position so new logs overwrite */
    last_kmsg_pos = 0;
    return NOTIFY_OK;
}

static struct notifier_block reboot_nb = {
    .notifier_call = last_kmsg_reboot,
};

static struct console last_kmsg_console = {
    .name = "lastkmsg",
    .write = last_kmsg_console_write,
    .flags = CON_ENABLED,
};

static int __init last_kmsg_init(void)
{
    struct device_node *np;
    struct resource res;

    /* find reserved memory from DT */
    np = of_find_compatible_node(NULL, NULL, "last-kmsg");
    if (!np) {
        pr_err("last_kmsg: no reserved memory\n");
        return -ENODEV;
    }

    of_address_to_resource(np, 0, &res);

    last_kmsg_buf = memremap(res.start, LAST_KMSG_SIZE, MEMREMAP_WB);
    if (!last_kmsg_buf)
        return -ENOMEM;

    marker = (u32 *)last_kmsg_buf;

    /* if marker invalid, init memory */
    if (*marker != MARKER_VALID)
        memset(last_kmsg_buf, 0, LAST_KMSG_SIZE);

    alloc_chrdev_region(&devno, 0, 1, DEVICE_NAME);
    cdev_init(&last_kmsg_cdev, &last_kmsg_fops);
    cdev_add(&last_kmsg_cdev, devno, 1);

    register_reboot_notifier(&reboot_nb);
    register_console(&last_kmsg_console);

    pr_info("last_kmsg: /dev/last_kmsg ready\n");
    return 0;
}

static void __exit last_kmsg_exit(void)
{
    unregister_console(&last_kmsg_console);
    unregister_reboot_notifier(&reboot_nb);
    cdev_del(&last_kmsg_cdev);
    unregister_chrdev_region(devno, 1);
}

module_init(last_kmsg_init);
module_exit(last_kmsg_exit);
MODULE_LICENSE("GPL");
