#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <asm/uaccess.h>


#define TIMER_TEST 1


typedef struct _dbug_page_ {
    struct _dbug_page_* next;
    int                 leng;
    char                data[PAGE_SIZE - (sizeof(void*)+sizeof(int))];
} DBUG_PAGE;


static struct dentry* dbug_root_dir = NULL;
static spinlock_t     dbug_lock;
static DBUG_PAGE*     dbug_head;
static DBUG_PAGE*     dbug_tail;
static int            dbug_pages = 0;
static int            dbug_max_pages = 64;
static u8             dbug_oneshot = 1;
static bool           dbug_eof = false;
static loff_t         dbug_off;
u32                   dbug_level = 0;
EXPORT_SYMBOL(dbug_level);


#if TIMER_TEST
static struct hrtimer hr_timer;
static u32 timer_interval_us = 256;
static u32 timer_ticker;


static enum hrtimer_restart timer_callback(struct hrtimer *timer)
{
    ktime_t currtime = ktime_get();
    ktime_t interval = ktime_set(0, timer_interval_us*1000); 
    hrtimer_forward(timer, currtime, interval);
    timer_ticker += 1;
    return HRTIMER_RESTART;
}


static int timer_init(void) 
{
    ktime_t ktime = ktime_set(0, timer_interval_us*1000);
    hrtimer_init(&hr_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);

    printk("Starting HR timer @ %dus\n", timer_interval_us);
    timer_ticker = 0;
    hr_timer.function = &timer_callback;
    hrtimer_start(&hr_timer, ktime, HRTIMER_MODE_REL);
    return 0;
}


static void timer_exit(void) 
{
    int ret = hrtimer_cancel(&hr_timer);
    if (ret) printk("HR timer was still in use...\n");
    printk("HR timer stopped\n");
}
#endif


static void dbug_init(void)
{
    spin_lock_init(&dbug_lock);
    dbug_head  = NULL;
    dbug_tail  = NULL;
    dbug_pages = 0;
}


static void _free_first_page(void)
{
    DBUG_PAGE* page;

    page = dbug_head;
    if (!page) {
        dbug_pages = 0;
        return;
    }
    dbug_pages--;
    dbug_head = dbug_head->next;
    if (dbug_head == NULL)
        dbug_tail = NULL;

    free_page((unsigned long) page);
}


static void free_first_page(void)
{
    unsigned long flags;

    spin_lock_irqsave(&dbug_lock, flags);
    _free_first_page();
    spin_unlock_irqrestore(&dbug_lock, flags);
}


static DBUG_PAGE*   append_new_page(void)
{
    DBUG_PAGE*    page;

    // if oneshot is enabled -- stop once all of the pages have been filled
    if (dbug_oneshot && dbug_pages >= dbug_max_pages) {
        return NULL;
    }
    page = (DBUG_PAGE*) get_zeroed_page(GFP_ATOMIC);

    // limit the size of the trace buffer based on dbug_max_pages setting
    while (dbug_pages >= dbug_max_pages && dbug_pages > 0) {
        _free_first_page();
    }

    if (page)
        ++dbug_pages;
    else {
        // could not allocate another page, reuse the first page on the list
        // if possible
        if (dbug_head == NULL)
            return NULL;
        if (dbug_head == dbug_tail) {
            memset(dbug_head, 0, PAGE_SIZE);
            return dbug_head;
        }
        page = dbug_head;
        dbug_head = dbug_head->next;
        if (dbug_head == NULL)
            dbug_tail = NULL;

        memset(page, 0, PAGE_SIZE);
    }

    if (dbug_head == NULL)
        dbug_head = page;
    else
        dbug_tail->next = page;
    dbug_tail = page;
    return page;
}


static void   append_new_data(char* text, int leng)
{
    unsigned long flags;

    spin_lock_irqsave(&dbug_lock, flags);
    do {
        if (dbug_tail && dbug_tail->leng + leng <= sizeof(dbug_tail->data)) {
            memcpy(dbug_tail->data + dbug_tail->leng, text, leng);
            dbug_tail->leng += leng;
            break;
        }
        if (append_new_page() == NULL)
            break;
    } while (dbug_tail != NULL);
    spin_unlock_irqrestore(&dbug_lock, flags);
}


int dbug_printk(const char* funcname, int lineno, const char* fmt, ...)
{
    char            text[ 256 ];
    int             leng;
    va_list         args;
    struct timeval  tv;

    do_gettimeofday(&tv);
    va_start(args, fmt);
    if (funcname == NULL) {
        leng = snprintf(text, sizeof(text), "%lu.%lu-", tv.tv_sec,tv.tv_usec);
    } else {
        leng = snprintf(text, sizeof(text), "%lu.%lu-%s().%d ", 
                        tv.tv_sec, tv.tv_usec, funcname, lineno);
    }
    leng += vsnprintf(text + leng, sizeof(text) - leng, fmt, args);
    va_end(args);
    append_new_data(text, leng);
    return leng;
}
EXPORT_SYMBOL(dbug_printk);


int dbug_hex_dump(const char* funcname, int lineno, const char* prefix_str, const void* buf, size_t len)
{
    const u8 *ptr = buf;
    int i, linelen, remaining = len, rowsize = 16, groupsize = 1;
    unsigned char linebuf[32 * 3 + 2 + 32 + 1];
    bool ascii = true;

    for (i = 0; i < len; i += rowsize) {
        linelen = min(remaining, rowsize);
        remaining -= rowsize;

        hex_dump_to_buffer(ptr + i, linelen, rowsize, groupsize, linebuf, sizeof(linebuf), ascii);
        dbug_printk(funcname, lineno, "%s%.8x: %s\n", prefix_str, i, linebuf);
    }
    return 0;
}
EXPORT_SYMBOL(dbug_hex_dump);


static int getval_from_user(struct file* file, 
                            const char* buf, 
                            u32 count, u32* value)
{
    int  len = count;
    char string[32];

    if (len > sizeof(string)-1) 
        len = sizeof(string)-1;
    if (copy_from_user(string, buf, len))
        return -EFAULT;

    string[len] = '\0';
    *value = simple_strtoul(string, NULL, 0);
    return 0;
}


static int trace_read(char* buf, size_t count, loff_t offset, bool* eof)
{
    DBUG_PAGE*     page;
    unsigned long  flags;
    size_t         leng;
    loff_t         off = offset;

    spin_lock_irqsave(&dbug_lock, flags);
    for (page = dbug_head; page != NULL; page = page->next) {
        if (off < page->leng)
            break;
        off -= page->leng;
    }

    if (page == NULL) {
        *eof = true;
        spin_unlock_irqrestore(&dbug_lock, flags);
        return 0;
    }

    leng = page->leng - off;
    if (leng > count)
        leng = count;
    memcpy(buf, page->data + off, leng);
    spin_unlock_irqrestore(&dbug_lock, flags);
    return leng;
}


static ssize_t trace_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos)
{
    int   len = count;
    char  string[256];

    if (len > sizeof(string)-1) 
        len = sizeof(string)-1;
    if (copy_from_user(string, buf, len))
        return -EFAULT;
    
    string[len] = '\0';
    return dbug_printk(NULL, 0, string);
}


static int open_pages(struct inode *inode, struct file *file)
{
    file->private_data = inode->i_private;
    return 0;
}


static ssize_t read_pages(struct file *file, char __user *buffer, size_t count, loff_t *ppos)
{
    char response[64];
    int  nbytes;

    if (*ppos) 
        return 0;
  
    nbytes = snprintf(response, sizeof(response), "%d of %d\n", dbug_pages, dbug_max_pages);
    if (nbytes > count)
        nbytes = count;

    if (copy_to_user(buffer, response, nbytes))
        return -EFAULT;
    *ppos += nbytes;
    return nbytes;
}


static ssize_t write_pages(struct file *file, const char __user *buffer, size_t count, loff_t *ppos)
{
    u32 value;
    int rc;

    if (*ppos)
        return 0;

    rc = getval_from_user(file, buffer, count, &value);
    if (rc)
        return rc;
   
    dbug_max_pages = value;
    *ppos += count;
    return count;
}


static struct file_operations pages_fops = {
    .owner = THIS_MODULE,
    .open  = open_pages,
    .read  = read_pages,
    .write = write_pages,
};


static int open_erase(struct inode *inode, struct file *file)
{
    file->private_data = inode->i_private;
    return 0;
}


static ssize_t write_erase(struct file *file, const char __user *buffer, size_t count, loff_t *ppos)
{
    while (dbug_head != NULL) {
        free_first_page();
    }
    return count;
}


static struct file_operations erase_fops = {
    .owner = THIS_MODULE,
    .open  = open_erase,
    .write = write_erase,
};


/**
 * This function is called at the beginning of a sequence.
 * ie, when:
 *	- the /proc file is read (first time)
 *	- after the function stop (end of sequence)
 *
 */
static void *trace_seq_start(struct seq_file *file, loff_t *pos)
{
    /* beginning a new sequence ? */	
    if (*pos == 0) {	
        /* yes => return a non null value to begin the sequence */
        *pos = 1;
        dbug_eof = false;
        dbug_off = 0;
        return &dbug_eof;
    } else {
        /* no => it's the end of the sequence, return end to stop reading */
        *pos = 0;
	return NULL;
    }
}


/**
 * This function is called after the beginning of a sequence.
 * It's called until the return is NULL (this ends the sequence).
 */
static void *trace_seq_next(struct seq_file *file, void *v, loff_t *pos)
{
    return dbug_eof? NULL : &dbug_eof;
}


/**
 * This function is called at the end of a sequence
 */
static void trace_seq_stop(struct seq_file *file, void *v)
{
    /* nothing to do, we use a static value in start() */
}


/**
 * This function is called for each "step" of a sequence
 */
static int trace_seq_show(struct seq_file *file, void *v)
{
    char buffer[256];
    int count = trace_read(buffer, sizeof(buffer)-1, dbug_off, &dbug_eof);
    if (count > 0) {
        if (count > sizeof(buffer)-1)
            count = sizeof(buffer)-1;
        buffer[count] = '\0';
        seq_printf(file, "%s", buffer);
        dbug_off += count;
    }
    return 0;
}


static struct seq_operations trace_seq_ops = {
    .start = trace_seq_start,
    .next  = trace_seq_next,
    .stop  = trace_seq_stop,
    .show  = trace_seq_show
};


static int trace_open(struct inode *inode, struct file *file)
{
    return seq_open(file, &trace_seq_ops);
};


static struct file_operations trace_fops = {
    .owner   = THIS_MODULE,
    .open    = trace_open,
    .write   = trace_write,
    .read    = seq_read,
    .llseek  = seq_lseek,
    .release = seq_release
};


static int open_test(struct inode *inode, struct file *file)
{
    file->private_data = inode->i_private;
    return 0;
}


static ssize_t read_test(struct file *file, char __user *buffer, size_t count, loff_t *ppos)
{
    char buf[128];
    int i;

    for (i = 0; i < sizeof(buf); ++i)
        buf[i] = i;
    dbug_hex_dump(__FUNCTION__, __LINE__, "BUF", buf, sizeof(buf));

    return 0;
}


static ssize_t write_test(struct file *file, const char __user *buffer, size_t count, loff_t *ppos)
{
    char buf[77];
    int i;

    for (i = 0; i < sizeof(buf); ++i)
        buf[i] = ' ' + i;
    dbug_hex_dump(__FUNCTION__, __LINE__, "buf", buf, sizeof(buf));
    return count;
}


static struct file_operations test_fops = {
    .owner = THIS_MODULE,
    .open  = open_test,
    .read  = read_test,
    .write = write_test,
};


static int open_timer_start(struct inode *inode, struct file *file)
{
    file->private_data = inode->i_private;
    return 0;
}


static ssize_t read_timer_start(struct file *file, char __user *buffer, size_t count, loff_t *ppos)
{
    timer_init();
    return 0;
}


static struct file_operations timer_start_fops = {
    .owner = THIS_MODULE,
    .open  = open_timer_start,
    .read  = read_timer_start,
};


static int open_timer_stop(struct inode *inode, struct file *file)
{
    file->private_data = inode->i_private;
    return 0;
}


static ssize_t read_timer_stop(struct file *file, char __user *buffer, size_t count, loff_t *ppos)
{
    timer_exit();
    return 0;
}


static struct file_operations timer_stop_fops = {
    .owner = THIS_MODULE,
    .open  = open_timer_stop,
    .read  = read_timer_stop,
};


void piper_dbug_exit(void)
{
    printk(KERN_EMERG "%s()\n", __FUNCTION__);
    if (dbug_root_dir != NULL) {
        debugfs_remove_recursive(dbug_root_dir);
        dbug_root_dir = NULL;
    }
}
EXPORT_SYMBOL(piper_dbug_exit);


int __init piper_dbug_init(void)
{
    printk(KERN_EMERG "%s()\n", __FUNCTION__);
    dbug_init();

    dbug_root_dir = debugfs_create_dir("piper-dbug", 0);
    if (dbug_root_dir == NULL)
        goto fail;
    if (!debugfs_create_u32("level", 0666, dbug_root_dir, &dbug_level))
        goto fail;
    if (!debugfs_create_u8("oneshot", 0666, dbug_root_dir, &dbug_oneshot))
        goto fail;
    if (!debugfs_create_file("pages", 0666, dbug_root_dir, NULL, &pages_fops))
        goto fail;
    if (!debugfs_create_file("erase", 0666, dbug_root_dir, NULL, &erase_fops))
        goto fail;
    if (!debugfs_create_file("trace", 0666, dbug_root_dir, NULL, &trace_fops))
        goto fail;
    if (!debugfs_create_file("test", 0666, dbug_root_dir, NULL, &test_fops))
        goto fail;

    #if TIMER_TEST
    if (!debugfs_create_u32("timer_interval_us", 0666, dbug_root_dir, &timer_interval_us))
        goto fail;
    if (!debugfs_create_u32("timer_ticker", 0666, dbug_root_dir, &timer_ticker))
        goto fail;
    if (!debugfs_create_file("timer_start", 0666, dbug_root_dir, NULL, &timer_start_fops))
        goto fail;
    if (!debugfs_create_file("timer_stop", 0666, dbug_root_dir, NULL, &timer_stop_fops))
        goto fail;
    #endif

    printk(KERN_EMERG "%s(): INITIALIZED!\n", __FUNCTION__);
    return 0;

fail:
    if (dbug_root_dir != NULL) {
        debugfs_remove_recursive(dbug_root_dir);
        dbug_root_dir = NULL;
    }
    
    printk(KERN_EMERG "%s(): FAILED!!\n", __FUNCTION__);
    return -1;
}
EXPORT_SYMBOL(piper_dbug_init);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Greg Hutchins");
