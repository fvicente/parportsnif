/*
 *  Create an input/output character device
 */
#include <linux/kernel.h>	/* We're doing kernel work */
#include <linux/module.h>	/* Specifically, a module */
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/parport.h>
#include <linux/ppdev.h>
#include <linux/smp_lock.h>
#include <linux/wait.h>
#include <linux/poll.h>
#include <linux/proc_fs.h>	/* Necessary because we use proc fs */
#include <linux/seq_file.h>	/* for seq_file */
#include <linux/file.h>
#include <asm/uaccess.h>	/* for get_user and put_user */

#define SUCCESS 0
#define BUF_LEN 80


#define CHRDEV      "parportsnif"
#define VPP_MAJOR   367
#define MAX_DEVICES 10
#define MAX_LINES   10000
#define PORT_NUM    0

DECLARE_WAIT_QUEUE_HEAD(readwait);

struct vpp_st {
    struct file *pfd;
    unsigned int port;
    int bufsz;
};

struct vlogline_st {
    struct vlogline_st *next;
    char *data;
    int size;
};

struct vlog_st {
    struct vlogline_st *lines;
    int totlines;
} vlog;

static struct class *vpp_class;

static ssize_t log_write(const char *buffer, size_t length)
{
    struct vlogline_st  *line, *newline;

	lock_kernel();
    line = vlog.lines;
    while(line && line->next)
        line = line->next;
    newline = (struct vlogline_st *)kzalloc(sizeof(struct vlogline_st), GFP_KERNEL);
    if (!newline) {
    	unlock_kernel();
        return -ENOMEM;
    }
    newline->data = (char *)kmalloc(length, GFP_KERNEL);
    if (!newline->data) {
    	unlock_kernel();
        kfree(newline);
        return -ENOMEM;
    }
    memcpy(newline->data, buffer, length);
    if(line)
        line->next = newline;
    else
        vlog.lines = newline;
    vlog.totlines++;
    while (vlog.totlines > MAX_LINES && vlog.lines) {
        line = vlog.lines;
        vlog.lines = line->next;
        kfree(line->data);
        kfree(line);
        vlog.totlines--;
    }
	unlock_kernel();
    return length;
}

static void vpp_log(struct vpp_st *vpp, const char *fmt, ...)
{
    char buf[900];
    va_list args;
    int sz;
    struct timespec ts;

    memset(&ts, 0, sizeof(ts));
    getnstimeofday(&ts);

    // current time
    sz = snprintf(buf, sizeof(buf), "[%u.%u] ", (unsigned int)ts.tv_sec, (unsigned int)ts.tv_nsec);
    
    // log message
    va_start(args, fmt);
    sz += vsnprintf(buf+sz, sizeof(buf)-sz, fmt, args);
    va_end(args);
    buf[sz++] = '\n';
    buf[sz] = '\0';
    log_write(buf, sz+1);
}

/* 
 * This is called whenever a process attempts to open the device file 
 */
static int device_open(struct inode *inode, struct file *file)
{
    struct vpp_st *vpp = NULL;
    char port[64];
    int err = 0;

	vpp = kmalloc (sizeof(struct vpp_st), GFP_KERNEL);
	if (!vpp) {
		return -ENOMEM;
    }
    vpp->bufsz = 512;
    vpp->port = iminor(inode);
    snprintf(port, sizeof(port)-1, "/dev/parport%d", vpp->port);
    /* open the real parallel port */
    vpp->pfd = filp_open(port, O_RDWR, 0);
    if (IS_ERR(vpp->pfd)) {
        err = PTR_ERR(vpp->pfd);
	    printk(KERN_ALERT "parportsnif: failed to open %s", port);
        return err;
    }
    file->private_data = vpp;
	try_module_get(THIS_MODULE);
    printk(KERN_INFO "parportsnif: successfully opened %s\n", port);
    vpp_log(vpp, "%d %s", vpp->port, "OPEN");
	return SUCCESS;
}

static int device_release(struct inode *inode, struct file *file)
{
    struct vpp_st *vpp = (struct vpp_st *) file->private_data;

    if (vpp->pfd) {
	    filp_close(vpp->pfd, NULL);
    }
    vpp->pfd = NULL;
	module_put(THIS_MODULE);
    printk(KERN_INFO "parportsnif: successfully closed /dev/parport%d\n", vpp->port);
    vpp_log(vpp, "%d %s", vpp->port, "CLOSE");
    kfree(vpp);
	return SUCCESS;
}

loff_t device_llseek(struct file *file, loff_t offset, int p)
{
    struct vpp_st *vpp = (struct vpp_st *) file->private_data;
    vpp_log(vpp, "%d %s", vpp->port, "SEEK");
    return(vpp->pfd->f_op->llseek(vpp->pfd, offset, p));
}

unsigned int device_poll(struct file *file, struct poll_table_struct *ps)
{
    struct vpp_st *vpp = (struct vpp_st *) file->private_data;
    vpp_log(vpp, "%d %s", vpp->port, "POLL");
    return(vpp->pfd->f_op->poll(vpp->pfd, ps));
}

/* 
 * This function is called whenever a process which has already opened the
 * device file attempts to read from it.
 */
static ssize_t device_read(struct file *file, char __user * buffer, size_t length, loff_t * offset)
{
    struct vpp_st *vpp = (struct vpp_st *) file->private_data;
    int i;
    char *buf;
    unsigned char ch;
    ssize_t bytes_read;

    printk(KERN_ALERT "%s\n", "parportsnif: starting read");
    buf = (char *)kmalloc((length*3)+1, GFP_KERNEL);
    if (!buf) {
        printk(KERN_ALERT "%s\n", "parportsnif: Failed to allocate read buffer");
        return 0;
    }
    *buf = '\0';
    bytes_read = vpp->pfd->f_op->read(vpp->pfd, buffer, length, offset);
    for (i = 0; i < bytes_read; i++, buffer++) {
        get_user(ch, buffer);
        snprintf(&buf[i*3], ((length-i)*3)+1, " %.2X", (unsigned int)ch);
    }
    vpp_log(vpp, "%d READ %d %s", vpp->port, bytes_read, buf);
    kfree(buf);
    return(bytes_read);
}

/* 
 * This function is called when somebody tries to
 * write into our device file. 
 */
static ssize_t
device_write(struct file *file,
	     const char __user * buffer, size_t length, loff_t * offset)
{
    struct vpp_st *vpp = (struct vpp_st *) file->private_data;
    int i;
    char *buf;
    unsigned char ch;
    ssize_t bytes_written;

    printk(KERN_ALERT "%s\n", "parportsnif: starting write");
    buf = (char *)kmalloc((length*3)+1, GFP_KERNEL);
    if (!buf) {
        printk(KERN_ALERT "%s\n", "parportsnif: Failed to allocate write buffer");
        return 0;
    }
    *buf = '\0';
    bytes_written = vpp->pfd->f_op->write(vpp->pfd, buffer, length, offset);
    for (i = 0; i < bytes_written; i++, buffer++) {
        get_user(ch, buffer);
        snprintf(&buf[i*3], ((length-i)*3)+1, " %.2X", (unsigned int)ch);
    }
    vpp_log(vpp, "%d WRITE %d %s", vpp->port, bytes_written, buf);
    kfree(buf);
	return(bytes_written);
}

#define LOGKFOP(el)     printk(KERN_INFO "vpp->pfd->f_op->%s = %p\n", #el, vpp->pfd->f_op->el)
#define CASE(a) case a: snprintf(buf, sizeof(buf)-1, "%s", #a)
/* 
 * This function is called whenever a process tries to do an ioctl on our
 * device file. We get two extra parameters (additional to the inode and file
 * structures, which all device functions get): the number of the ioctl called
 * and the parameter given to the ioctl function.
 *
 * If the ioctl is write or read/write (meaning output is returned to the
 * calling process), the ioctl call returns the output of this function.
 *
 */
long device_unlocked_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct vpp_st *vpp = (struct vpp_st *) file->private_data;
	int has_data=0;
	unsigned char ch;
    char buf[64];

	/* 
	 * Switch according to the ioctl called 
	 */
    switch(cmd) {
    CASE(PPSETMODE);
        break;
    CASE(PPRSTATUS);
        break;
    CASE(PPRCONTROL);
        break;
    CASE(PPWCONTROL);
        break;
    CASE(PPFCONTROL);
        break;
    CASE(PPRDATA);
        break;
    CASE(PPWDATA);
        get_user(ch, (char *)arg);
        has_data = 1;
        break;
    CASE(PPCLAIM);
        break;
    CASE(PPRELEASE);
        break;
    CASE(PPYIELD);
        break;
    CASE(PPEXCL);
        break;
    CASE(PPDATADIR);
        break;
    CASE(PPNEGOT);
        break;
    CASE(PPWCTLONIRQ);
        break;
    CASE(PPCLRIRQ);
        break;
    CASE(PPSETPHASE);
        break;
    CASE(PPGETTIME);
        break;
    CASE(PPSETTIME);
        break;
    CASE(PPGETMODES);
        break;
    CASE(PPGETMODE);
        break;
    CASE(PPGETPHASE);
        break;
    CASE(PPGETFLAGS);
        break;
    CASE(PPSETFLAGS);
        break;
    CASE(PP_FASTWRITE);
        break;
    CASE(PP_FASTREAD);
        break;
    CASE(PP_W91284PIC);
        break;
    default:
        snprintf(buf, sizeof(buf)-1, "UNKNOWN %u", cmd);
        break;
    }
    if(has_data) {
        vpp_log(vpp, "%d IOCTL %s %.2X", vpp->port, buf, (unsigned int)ch);
    } else {
        vpp_log(vpp, "%d IOCTL %s", vpp->port, buf);
    }
    return(vpp->pfd->f_op->unlocked_ioctl(vpp->pfd, cmd, arg));
}

/* log file operations */

/**
 * This function is called at the beginning of a sequence.
 * ie, when:
 *	- the /proc file is read (first time)
 *	- after the function stop (end of sequence)
 *
 */
static void *log_seq_start(struct seq_file *s, loff_t *pos)
{
	static unsigned long counter = 0;

	/* beginning a new sequence ? */	
	if ( *pos == 0 )
	{	
		/* yes => return a non null value to begin the sequence */
		return &counter;
	}
	else
	{
		/* no => it's the end of the sequence, return end to stop reading */
		*pos = 0;
		return NULL;
	}
}

/**
 * This function is called after the beginning of a sequence.
 * It's called untill the return is NULL (this ends the sequence).
 *
 */
static void *log_seq_next(struct seq_file *s, void *v, loff_t *pos)
{
	return vlog.lines;
}

/**
 * This function is called at the end of a sequence
 * 
 */
static void log_seq_stop(struct seq_file *s, void *v)
{
	/* nothing to do, we use a static value in start() */
}

/**
 * This function is called for each "step" of a sequence
 *
 */
static int log_seq_show(struct seq_file *s, void *v)
{
    struct vlogline_st  *line = NULL;

    if (!vlog.lines) {
    	return 0;
    }
	lock_kernel();
    line = vlog.lines;
    if (!line) {
        unlock_kernel();
        return 0;
    }
    seq_printf(s, "%s", line->data);
    vlog.lines = line->next;
    kfree(line->data);
    kfree(line);
    vlog.totlines--;
	unlock_kernel();
	return 0;
}

/**
 * This structure gather "function" to manage the sequence
 *
 */
static struct seq_operations log_seq_ops = {
	.start = log_seq_start,
	.next  = log_seq_next,
	.stop  = log_seq_stop,
	.show  = log_seq_show
};

/**
 * This function is called when the /proc file is open.
 *
 */
static int log_open(struct inode *inode, struct file *file)
{
	return seq_open(file, &log_seq_ops);
};

/* Module Declarations */

/* 
 * This structure will hold the functions to be called
 * when a process does something to the device we
 * created. Since a pointer to this structure is kept in
 * the devices table, it can't be local to
 * init_module. NULL is for unimplemented functions. 
 */
struct file_operations vpp_fops = {
    .owner = THIS_MODULE,
    .llseek = device_llseek,
	.read = device_read,
	.write = device_write,
    .poll = device_poll,
	.unlocked_ioctl = device_unlocked_ioctl,
	.open = device_open,
	.release = device_release,	/* a.k.a. close */
};

struct file_operations vlog_fops = {
	.owner   = THIS_MODULE,
	.open    = log_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = seq_release
};

static void vpp_attach(struct parport *port)
{
    struct device *device;
	int err = 0;

	device = device_create(vpp_class, port->dev, MKDEV(VPP_MAJOR, port->number),
		      NULL, "parportsnif%d", port->number);
    if (IS_ERR(device)) {
        err = PTR_ERR(device);
        printk(KERN_WARNING CHRDEV "Error %d while trying to create %s%d", err, CHRDEV, port->number);
    }
}

static void vpp_detach(struct parport *port)
{
	device_destroy(vpp_class, MKDEV(VPP_MAJOR, port->number));
}

static struct parport_driver vpp_driver = {
	.name		= CHRDEV,
	.attach		= vpp_attach,
	.detach		= vpp_detach,
};

static int __init vpp_init (void)
{
	int err = 0;
	struct proc_dir_entry *entry;

    memset(&vlog, 0, sizeof(vlog));

	if (register_chrdev (VPP_MAJOR, CHRDEV, &vpp_fops)) {
		printk (KERN_WARNING CHRDEV ": unable to get major %d\n",
			VPP_MAJOR);
		return -EIO;
	}
	vpp_class = class_create(THIS_MODULE, CHRDEV);
	if (IS_ERR(vpp_class)) {
		printk (KERN_WARNING CHRDEV ": error creating class parportsnif\n");
		err = PTR_ERR(vpp_class);
		goto out_chrdev;
	}
	if (parport_register_driver(&vpp_driver)) {
		printk (KERN_WARNING CHRDEV ": unable to register with parport\n");
		goto out_class;
	}

    /* register log file in /proc */
	entry = create_proc_entry("parportlog", 0, NULL);
	if (entry) {
		entry->proc_fops = &vlog_fops;
	}

	goto out;

out_class:
	class_destroy(vpp_class);
out_chrdev:
	unregister_chrdev(VPP_MAJOR, CHRDEV);
out:
	return err;
}

static void __exit vpp_cleanup (void)
{
	/* Clean up all parport stuff */
    remove_proc_entry("parportlog", NULL);
	parport_unregister_driver(&vpp_driver);
	class_destroy(vpp_class);
	unregister_chrdev (VPP_MAJOR, CHRDEV);
}

module_init(vpp_init);
module_exit(vpp_cleanup);

MODULE_LICENSE("GPL");
MODULE_ALIAS_CHARDEV_MAJOR(VPP_MAJOR);

