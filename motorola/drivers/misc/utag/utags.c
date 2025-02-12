/* Copyright (c) 2012, Motorola Mobility LLC. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#define pr_fmt(fmt) "utags (%s): " fmt, __func__

#include <linux/fcntl.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/ioctl.h>
#include <linux/input.h>
#include <linux/list.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/platform_device.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/string.h>
#include <linux/stat.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/unistd.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/of.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/completion.h>
#include <linux/workqueue.h>
#include <linux/version.h>
#include <linux/blk_types.h>

#define MAX_UTAG_SIZE 1024
#define MAX_UTAG_NAME 32
#define UTAG_DEPTH 5
#define UTAG_HEAD  "__UTAG_HEAD__"
#define UTAG_TAIL  "__UTAG_TAIL__"
#define ROUNDUP(a, b) (((a) + ((b)-1)) & ~((b)-1))
#define TO_SECT_SIZE(n)     (((n) + 511) & ~511)
#define DRVNAME "utags"
#define DEFAULT_ROOT "config"
#define HW_ROOT "hw"

struct ctrl;

enum utag_flag {
	UTAG_FLAG_PROTECTED = 1 << 0,
};

#define UTAG_STATUS_LOADED '0'
#define UTAG_STATUS_RELOAD '1'
#define UTAG_STATUS_NOT_READY '2'
#define UTAG_STATUS_FAILED '3'

struct utag {
	char name[MAX_UTAG_NAME]; /* UTAG name and type combined */
	char name_only[MAX_UTAG_NAME]; /* UTAG name with type removed */
	uint32_t size;
	uint32_t flags;
	uint32_t util;
	void *payload;
	struct utag *next;
	struct utag *prev;
};

struct frozen_utag {
	char name[MAX_UTAG_NAME];
	uint32_t size;
	uint32_t flags;
	uint32_t util;
	uint8_t payload[];
};

#define UTAG_MIN_TAG_SIZE   (sizeof(struct frozen_utag))

enum utag_output {
	OUT_ASCII = 0,
	OUT_RAW,
	OUT_TYPE
};

static char *files[] = {
	"ascii",
	"raw",
	"type"
};

struct proc_node {
	struct list_head entry;
	char name[MAX_UTAG_NAME]; /* UTAG name string */
	char type[MAX_UTAG_NAME]; /* UTAG type string */
	char file_name[MAX_UTAG_NAME];
	struct proc_dir_entry *file;
	struct proc_dir_entry *dir;
	uint32_t mode;
	struct ctrl *ctrl;
};

struct dir_node {
	struct list_head entry;
	char name[MAX_UTAG_NAME];
	char path[MAX_UTAG_NAME];
	struct proc_dir_entry *dir;
	struct proc_dir_entry *parent;
	struct ctrl *ctrl;
};

struct blkdev {
	const char *name;
	struct file *filep;
	size_t size;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0) || defined(CONFIG_MMI_UTAG_RW_BIO)
	struct block_device *bdev;
#endif
};

struct ctrl {
	struct blkdev main;
	struct blkdev backup;
	struct platform_device *pdev;
	struct proc_dir_entry *root;
	char reload;
	size_t rsize;
	struct list_head dir_list;
	struct list_head node_list;
	const char *dir_name;
	uint32_t lock;
	uint32_t hwtag;
	struct mutex access_lock;
	struct utag *attrib;
	struct utag *features;
	struct completion load_comp;
	struct completion store_comp;
	struct workqueue_struct *load_queue;
	struct workqueue_struct *store_queue;
	struct work_struct load_work;
	struct work_struct store_work;
	struct utag *head;
	int store_work_result;
};

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0) || defined(CONFIG_MMI_UTAG_RW_BIO)
#include <linux/blkdev.h>
#include <linux/bio.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
#define PDE_DATA(x) pde_data(x)
#endif

static struct page *addr_to_page(void *addr)
{
	if (is_vmalloc_addr(addr))
		return vmalloc_to_page(addr);
	else
		return virt_to_page(addr);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0)
#define NR_BIO_MAX_PAGES BIO_MAX_VECS
#else
#define NR_BIO_MAX_PAGES BIO_MAX_PAGES
#endif

static int utags_submit_bio(struct block_device *bdev, void *buf, int pages, int opf)
{
	int i, ret;
	struct bio *bio;
	int num, left_pages = pages;

	pr_debug("%s: pages %d left_pages %d begin\n", __func__, pages, left_pages);
	while (left_pages > 0) {
		num = (left_pages >= NR_BIO_MAX_PAGES) ? NR_BIO_MAX_PAGES : left_pages;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
		bio = bio_alloc(bdev, num, 0, GFP_KERNEL);
#else
		bio = bio_alloc(GFP_KERNEL, num);
#endif
		if (!bio)
			return -ENOMEM;

		bio->bi_iter.bi_sector = (pages - left_pages) * (PAGE_SIZE >> 9);
		bio->bi_opf = opf;
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 1, 0)
		bio_set_dev(bio, bdev);
#endif

		for (i = 0; i < num; i++) {
			if (!bio_add_page(bio, addr_to_page(buf + (pages - left_pages + i) * PAGE_SIZE), PAGE_SIZE, 0)) {
				bio_put(bio);
				return -EIO;
			}
		}

		ret = submit_bio_wait(bio);
		if (ret)
			pr_err("Submit bio err %d,%d,%d", num, opf, ret);

		bio_put(bio);
		left_pages -= num;
		pr_debug("%s: pages %d left_pages %d\n", __func__, pages, left_pages);
	}
	return ret;
}

static ssize_t rw_bdev(struct block_device *bdev, void *buf, size_t count, int opf)
{
	int ret;

	ret = utags_submit_bio(bdev, buf, 1 << get_order(count), opf);
	return  ret < 0 ? ret : count;
}

static ssize_t kernel_read_stub(struct blkdev* cb, void *buf, size_t count)
{
	return rw_bdev(cb->bdev, buf, count, REQ_OP_READ);
}
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(4, 14, 0)
static inline ssize_t kernel_read_stub(struct blkdev* cb, void *addr, size_t count)
{
	loff_t pos = 0;
	return kernel_read(cb->filep, addr, count, &pos);
}
#else
static inline ssize_t kernel_read_stub(struct blkdev* cb, void *addr, size_t count)
{
	return kernel_read(cb->filep, 0, addr, count);
}
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0) || defined(CONFIG_MMI_UTAG_RW_BIO)
static ssize_t kernel_write_stub(struct blkdev* cb, void *buf, size_t count)
{
	return rw_bdev(cb->bdev, buf, count, REQ_OP_WRITE | REQ_SYNC);
}
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(5, 4, 0)
static inline ssize_t kernel_write_stub(struct blkdev* cb, void *addr, size_t count)
{
	loff_t pos = 0;
	return kernel_write(cb->filep, addr, count, &pos);
}
#else
static inline ssize_t kernel_write_stub(struct blkdev* cb, void *addr, size_t count)
{
	loff_t pos = 0;
	return vfs_write(cb->filep, addr, count, &pos);
}
#endif

static int build_utags_directory(struct ctrl *ctrl);
static void clear_utags_directory(struct ctrl *ctrl);

static int store_utags(struct ctrl *ctrl, struct utag *tags);

static ssize_t write_utag(struct file *file, const char __user *buffer,
	   size_t count, loff_t *pos);
static ssize_t new_utag(struct file *file, const char __user *buffer,
	   size_t count, loff_t *pos);
static ssize_t delete_utag(struct file *file, const char __user *buffer,
	   size_t count, loff_t *pos);
static int add_utag_tail(struct utag *head, char *utag_name, char *utag_type);

static int lock_open(struct inode *inode, struct file *file);
static int partition_open(struct inode *inode, struct file *file);

#if KERNEL_VERSION(5, 10, 0) <= LINUX_VERSION_CODE
static const struct proc_ops utag_fops = {
	.proc_open = partition_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
	.proc_write = write_utag,
};

static const struct proc_ops new_fops = {
	.proc_read = NULL,
	.proc_write = new_utag,
};

static const struct proc_ops lock_fops = {
	.proc_open = lock_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static const struct proc_ops delete_fops = {
	.proc_read = NULL,
	.proc_write = delete_utag,
};
#else
static const struct file_operations utag_fops = {
	.owner = THIS_MODULE,
	.open = partition_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
	.write = write_utag,
};

static const struct file_operations new_fops = {
	.read = NULL,
	.write = new_utag,
};

static const struct file_operations lock_fops = {
	.open = lock_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static const struct file_operations delete_fops = {
	.read = NULL,
	.write = delete_utag,
};
#endif

/*
 * check utag name
 * for special symbols
 *
 */

static int no_show_tag(char *name)
{
	if (('/' == name[0]) && (0 == name[1])) {
		pr_err("skipping bl root tag");
		return 1;
	}

	if (('.' == name[0]) && (0 == name[1])) {
		pr_err("skipping . tag");
		return 1;
	}

	return 0;
}

/*
 * Read tags head
 */

static int read_head(struct blkdev *cb, struct utag *htag)
{
	int bytes;
	struct frozen_utag *buf;

	if (!htag) {
		pr_err("[%s] null pointer", cb->name);
		return -EINVAL;
	}

	buf = vmalloc(UTAG_MIN_TAG_SIZE);
	if (!buf)
		return -ENOMEM;

	bytes = kernel_read_stub(cb, (void *) buf, UTAG_MIN_TAG_SIZE);
	if ((int) UTAG_MIN_TAG_SIZE > bytes) {
		pr_err("ERR file (%s) read failed ret %d\n", cb->name, bytes);
		vfree(buf);
		return -EIO;
	}

	strlcpy(htag->name, buf->name, MAX_UTAG_NAME - 1);
	if (strncmp(htag->name, UTAG_HEAD, MAX_UTAG_NAME)) {
		pr_err("[%s] invalid or empty utags partition\n", cb->name);
		vfree(buf);
		return -EIO;
	}
	htag->flags = ntohl(buf->flags);
	htag->util = ntohl(buf->util);
	htag->size = ntohl(buf->size);
	pr_debug("utag file (%s) flags %#x util %#x size %#x\n",
		cb->name, htag->flags, htag->util, htag->size);
	vfree(buf);
	return 0;
}

/*
 * Initialize empty tags
 *
 */
static int init_empty(struct ctrl *ctrl)
{
	struct utag *htag;

	pr_err("erasing [%s]\n", ctrl->dir_name);
	htag = kzalloc(sizeof(struct utag), GFP_KERNEL);
	if (!htag)
		return -ENOMEM;

	strlcpy(htag->name, UTAG_HEAD, MAX_UTAG_NAME);
	add_utag_tail(htag, UTAG_TAIL, NULL);
	ctrl->head = htag;
	queue_work(ctrl->store_queue, &ctrl->store_work);
	wait_for_completion(&ctrl->store_comp);
	kfree(htag);
	return 0;
}

static char *get_dir_name(struct ctrl *ctrl, struct proc_dir_entry *dir)
{
	struct dir_node *c, *dir_node = NULL;

	list_for_each_entry(c, &ctrl->dir_list, entry) {
		if (c->dir == dir) {
			dir_node = c;
			break;
		}
	}
	return dir_node ? dir_node->name : "na";
}

/*
 * Check util field of head utag for actual data size
 *
 */
static size_t data_size(struct blkdev *cb)
{
	size_t bytes;
	struct utag htag;
	struct ctrl *ctrl = container_of(cb, struct ctrl, main);

	memset(&htag, 0, sizeof(struct utag));
	if (read_head(cb, &htag))
		return 0;

	ctrl->lock = htag.flags & UTAG_FLAG_PROTECTED ? 1 : 0;
	bytes = htag.util;
	pr_debug("file (%s) saved %zu block %zu flags %#x ctrl %zu\n",
		cb->name, bytes, cb->size, htag.flags, ctrl->rsize);

	/* On the first read always load entire partition */
	if (ctrl->hwtag) {
		pr_debug("[%s] hwtag sizes\n", ctrl->dir_name);
		if (!ctrl->rsize || !bytes || bytes > cb->size)
			bytes = cb->size;
	} else
			bytes = cb->size;

	ctrl->rsize = bytes;

	pr_debug("[%s] reading %zu bytes\n", ctrl->dir_name, bytes);
	return bytes;
}

/*
 * Open and store file handle for a utag partition
 *
 * Not thread safe, call from a safe context only
 */
static int open_utags(struct blkdev *cb)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0) || defined(CONFIG_MMI_UTAG_RW_BIO)
	struct block_device *bdev = NULL;

	bdev = blkdev_get_by_path(cb->name, FMODE_READ | FMODE_WRITE, cb);

	if (IS_ERR(bdev)) {
		pr_err("(%s) failed get block device\n", cb->name);
		return -EIO;
	}

	cb->bdev = bdev;
	cb->size = i_size_read(bdev->bd_inode);
	cb->filep = NULL;
	pr_debug("%s: read inode size %zu\n", __func__, cb->size);
#else
	struct inode *inode = NULL;

	if (!cb->name)
		return -EIO;

	if (cb->filep)
		return 0;

	cb->filep = filp_open(cb->name, O_RDWR|O_SYNC, 0600);
	if (IS_ERR_OR_NULL(cb->filep)) {
		int rc = PTR_ERR(cb->filep);
		if (rc == -EROFS) {
			pr_info("[%s] readonly, open as O_RDONLY\n", cb->name);
			cb->filep = filp_open(cb->name, O_RDONLY|O_SYNC, 0600);
			if (IS_ERR_OR_NULL(cb->filep)) {
				rc = PTR_ERR(cb->filep);
				pr_err("opening (%s) errno=%d\n", cb->name, rc);
				cb->filep = NULL;
				return rc;
			}
		} else {
			pr_err("opening (%s) errno=%d\n", cb->name, rc);
			cb->filep = NULL;
			return rc;
		}
	}

	if (cb->filep->f_path.dentry)
		inode = cb->filep->f_path.dentry->d_inode;
	if (!inode || !S_ISBLK(inode->i_mode)) {
		pr_err("(%s) not a block device\n", cb->name);
		filp_close(cb->filep, NULL);
		cb->filep = NULL;
		return -EIO;
	}

	cb->size = i_size_read(inode->i_bdev->bd_inode);
#endif
	pr_debug("[%s] (pid %i) open (%s) success\n",
		current->comm, current->pid, cb->name);
	return 0;
}


/*
 * Free the memory associated with a list of tags.
 *
 */

static inline void free_tags(struct utag *tags)
{
	struct utag *next;

	while (tags) {
		next = tags->next;
		kfree(tags->payload);
		kfree(tags);
		tags = next;
	}
}

static void walk_tags(struct utag *tags)
{
	struct utag *next;

	while (tags) {
		next = tags->next;
		pr_debug("utag [%s], payload size %u\n",
			tags->name, tags->size);
		tags = next;
	}
}

static inline void walk_proc_nodes(struct ctrl *ctrl)
{
	struct proc_node *p;

	list_for_each_entry(p, &ctrl->node_list, entry) {
		pr_debug("proc-node [%s:%s:%s]\n",
			p->name, p->type, p->file_name);
	}
}

static inline void walk_dir_nodes(struct ctrl *ctrl)
{
	struct dir_node *d;

	list_for_each_entry(d, &ctrl->dir_list, entry) {
		pr_debug("dir-node [%s] path [%s]\n", d->name, d->path);
	}
}

/*
 * compare names, break check at any null in names
 * returns true if match, false otherwise
 */

static inline bool names_match(const char *s1, const char *s2)
{
	register size_t count = MAX_UTAG_NAME;
	register int r, c1, c2;

	pr_debug("cmp (%s) <=> (%s)\n)", s1, s2);
	while (count--) {
		c1 = *s1++;
		c2 = *s2++;
		r = c1 - c2;
		if (r || !c1 || !c2)
			return (r) ? false : true;
	}
	return true;
}

/*
 *
 * Check for name to have single ':' char
 * not in the first or last position
 *
 * returns true if name is OK, false otherwise
 */

static inline bool validate_name(const char *s1, int count)
{
	register int c1 = *s1, sep = 0;

	if (c1 == ':')
		return false;
	while (count--) {
		if (c1 == ':')
			sep++;
		if (sep > 1)
			return false;
		c1 = *s1++;
	}
	if (c1 == ':')
		return false;
	return true;
}

/*
 * validate path to have :type as a last element
 */

static int check_path(char *fullpath, int count)
{
	char *ptr, *bptr;

	ptr = strnchr(fullpath, count, ':');
	bptr = strnchr(fullpath, count, '/');
	if (bptr && ptr && bptr > ptr) {
		pr_err("Invalid path %s\n", fullpath);
		return -EINVAL;
	}

	return 0;
}


/*
 * Extract hierarchical names from the fullpath
 * Return: number of pointers in name array
 */

static int full_split(char *fullpath, char **name, char **type)
{
	int i;
	char *ptr;

	ptr = strnchr(fullpath, MAX_UTAG_NAME, ':');
	if (ptr) {
		*ptr++ = 0;
		*type = ptr;
		pr_debug("type=%s\n", *type);
	}

	for (i = 0, ptr = fullpath; ptr; i++) {
		if (*ptr == '/')
			*ptr++ = 0;
		name[i] = ptr;
		ptr = strnchr(ptr, MAX_UTAG_NAME, '/');
		pr_debug("name[%d]=%s\n", i, name[i]);
	}

	return i;
}

static int add_utag_tail(struct utag *head, char *utag_name, char *utag_type)
{
	char utag[MAX_UTAG_NAME];
	struct utag *new, *tail;

	scnprintf(utag, MAX_UTAG_NAME, "%s%s%s", utag_name,
		utag_type ? ":" : "", utag_type ? utag_type : "");

	/* find list tail and check for duplicate utag */
	for (tail = head; tail->next; tail = tail->next) {
		if (!strncmp(tail->name_only, utag, MAX_UTAG_NAME)) {
			pr_debug("utag [%s] already exists\n", utag);
			return -EEXIST;
		}
	}
	pr_debug("tail utag [%s]\n", tail->name);

	new = kzalloc(sizeof(struct utag), GFP_KERNEL);
	if (!new)
		return -ENOMEM;

	strlcpy(new->name, utag, MAX_UTAG_NAME);
	new->size = new->flags = new->util = 0;

	if (!tail->prev) { /* tail is in fact the head */
		tail->next = new;
		new->prev = tail;
	} else {
		/* insert new utag before tail */
		new->prev = tail->prev;
		new->next = tail;
		tail->prev->next = new;
		tail->prev = new;
	}
	walk_tags(head);

	return 0;
}

/*
 * Find first instance of utag by specified name
 */

static struct utag *find_first_utag(const struct utag *head, const char *name)
{
	struct utag *cur;

	if (!head)
		return NULL;

	cur = head->next;	/* skip HEAD */
	while (cur) {
		/* skip TAIL */
		if (cur->next == NULL)
			break;
		if (names_match(name, cur->name))
			return cur;
		cur = cur->next;
	}
	return NULL;
}

/*
 * Create, initialize add to the list procfs utag file node
 */

static int proc_utag_file(char *utag_name, char *utag_type,
	  enum utag_output mode, struct dir_node *dnode,
#if KERNEL_VERSION(5, 10, 0) <= LINUX_VERSION_CODE
	  const struct proc_ops *fops)
#else
	  const struct file_operations *fops)
#endif
{
	struct proc_node *node;
	struct ctrl *ctrl = dnode->ctrl;

	if (sizeof(files) < mode)
		return -EINVAL;

	node = kzalloc(sizeof(struct proc_node), GFP_KERNEL);
	if (node) {
		list_add_tail(&node->entry, &ctrl->node_list);
		strlcpy(node->file_name, files[mode], MAX_UTAG_NAME);
		strlcpy(node->name, utag_name, MAX_UTAG_NAME);
		if (utag_type)
			strlcpy(node->type, utag_type, MAX_UTAG_NAME);
		else
			node->type[0] = 0;
		node->mode = mode;
		node->dir = dnode->dir;
		node->ctrl = ctrl;
		node->file = proc_create_data(node->file_name, 0660,
			dnode->dir, fops, node);

		pr_debug("created file [%s/%s]\n",
			get_dir_name(ctrl, dnode->dir), node->file_name);
	}
	return 0;
}

static struct dir_node *find_dir_node(struct ctrl *ctrl, char *path)
{
	struct dir_node *c, *dir_node = NULL;

	list_for_each_entry(c, &ctrl->dir_list, entry) {
		if (!strncmp(c->path, path, MAX_UTAG_NAME)) {
			dir_node = c;
			break;
		}
	}
	return dir_node;
}

static int proc_utag_util(struct ctrl *ctrl)
{
	struct proc_dir_entry *dir;

	dir = proc_mkdir_data("all", 0771, ctrl->root, NULL);
	if (!dir) {
		pr_err("failed to create util\n");
		return -EIO;
	}

	if (!proc_create_data("new", 0220, dir, &new_fops, ctrl)) {
		pr_err("Failed to create utag new entry\n");
		return -EIO;
	}

	if (!proc_create_data("lock", 0660, dir, &lock_fops, ctrl)) {
		pr_err("Failed to create lock entry\n");
		return -EIO;
	}

	if (!proc_create_data(".delete", 0220, dir, &delete_fops, ctrl)) {
		pr_err("Failed to create delete entry\n");
		return -EIO;
	}

	return 0;
}

static struct proc_dir_entry *proc_utag_dir(struct ctrl *ctrl,
	char *tname, char *path, char *ttype, bool populate,
	struct proc_dir_entry *parent)
{
	struct dir_node *dnode;
	struct proc_dir_entry *dir = NULL;

	if (!parent)
		parent = ctrl->root;

	dnode = find_dir_node(ctrl, path);
	if (dnode) {
		if (populate)
			goto populate_utag_dir;

		pr_debug("procfs dir %s exists; skip\n", tname);
		return dnode->dir;
	}

	dir = proc_mkdir_data(tname, 0771, parent, NULL);
	if (!dir) {
		pr_err("failed to create dir %s\n", tname);
		return ERR_PTR(-ENOMEM);
	}

	dnode = kzalloc(sizeof(struct dir_node), GFP_KERNEL);
	if (!dnode) {
		kfree(dir);
		pr_err("failed to create node structure\n");
		return ERR_PTR(-ENOMEM);
	}

	dnode->parent = parent;
	dnode->ctrl = ctrl;
	dnode->dir = dir;
	list_add_tail(&dnode->entry, &ctrl->dir_list);
	strlcpy(dnode->name, tname, MAX_UTAG_NAME);
	strlcpy(dnode->path, path, MAX_UTAG_NAME);

	if (!populate)
		return dir;

populate_utag_dir:

	proc_utag_file(tname, ttype, OUT_ASCII, dnode, &utag_fops);
	proc_utag_file(tname, ttype, OUT_RAW, dnode, &utag_fops);
	proc_utag_file(tname, ttype, OUT_TYPE, dnode, &utag_fops);

	return dir;
}

/*
 * Convert a block of tags, presumably loaded from seconday storage, into a
 * format that can be manipulated.
 */
static struct utag *thaw_tags(size_t block_size, void *buf)
{
	struct utag *head = NULL, *cur = NULL;
	uint8_t *ptr = buf;

	while (1) {
		struct frozen_utag *frozen;
		uint8_t *next_ptr;
		char *sep;

		frozen = (struct frozen_utag *)ptr;

		if (!head) {
			/* This is allocation of the head */
			cur = kzalloc(sizeof(struct utag), GFP_KERNEL);
			if (!cur)
				return NULL;
		}

		strlcpy(cur->name, frozen->name, MAX_UTAG_NAME - 1);
		strlcpy(cur->name_only, frozen->name, MAX_UTAG_NAME-1);
		sep = strnchr(cur->name_only, MAX_UTAG_NAME, ':');
		if (sep)
			*sep = 0;
		cur->flags = ntohl(frozen->flags);
		cur->util = ntohl(frozen->util);
		cur->size = ntohl(frozen->size);

		if (!head) {
			head = cur;

			if (strcmp(head->name, UTAG_HEAD)) {
				pr_err("invalid or empty utags partition\n");
				goto err_free;
			}
		}

		/* moved here to print statistics for tail as well */
		next_ptr = ptr + UTAG_MIN_TAG_SIZE + ROUNDUP(cur->size, 4);
		pr_debug("utag [%s] size %zu\n", cur->name, next_ptr - ptr);

		/* check if this is the end */
		if (!strcmp(cur->name, UTAG_TAIL)) {
			/* footer payload size should be zero */
			if (0 != cur->size) {
				pr_err("invalid utags tail\n");
				goto err_free;
			}

			/* all done */
			break;
		}

		/*
		 * Ensure there is enough space in the buffer for both the
		 * payload and the tag header for the next tag.
		 */
		if ((next_ptr - (uint8_t *) buf) + UTAG_MIN_TAG_SIZE >
		    block_size) {
			pr_err("invalid tags size\n");
			goto err_free;
		}

		if (cur->size != 0) {
			cur->payload = kzalloc(cur->size + 1, GFP_KERNEL);
			if (!cur->payload)
				goto err_free;
			memcpy(cur->payload, frozen->payload, cur->size);
		}

		/* advance to beginning of next tag */
		ptr = next_ptr;

		/* get ready for the next tag */
		cur->next = kzalloc(sizeof(struct utag), GFP_KERNEL);
		/* FIXME if kzalloc fails, kernel will panic in the next line */
		cur->next->prev = cur;
		cur = cur->next;
		if (!cur)
			goto err_free;
	} /* while (1) */

	walk_tags(head);
	goto out;

 err_free:
	free_tags(head);
	head = NULL;
 out:
	return head;
}

static void *freeze_tags(size_t block_size, struct utag *tags,
	size_t *tags_size)
{
	size_t written, frozen_size = 0;
	char *buf = NULL, *ptr;
	struct utag *cur = tags;
	size_t zeros;
	struct frozen_utag frozen = { {0} };

	/* Make sure the tags start with the HEAD marker. */
	if (!tags || strncmp(tags->name, UTAG_HEAD, MAX_UTAG_NAME)) {
		pr_err("invalid utags head\n");
		return NULL;
	}

	/*
	 * Walk the list once to determine the amount of space to allocate
	 * for the frozen tags.
	 */
	while (cur) {
		pr_debug("utag [%s], payload size %u\n", cur->name, cur->size);
		frozen_size += ROUNDUP(cur->size, 4) + UTAG_MIN_TAG_SIZE;
		pr_debug("calculated size %zu\n", frozen_size);
		if (!strncmp(cur->name, UTAG_TAIL, MAX_UTAG_NAME))
			break;
		cur = cur->next;
	}

	/* round up frozen_size to eMMC sector size */
	frozen_size = TO_SECT_SIZE(frozen_size);
	pr_debug("frozen size aligned to sector size %zu\n", frozen_size);

	/* do some more sanity checking */
	if (!cur || cur->next) {
		pr_err("utags corrupted\n");
		return NULL;
	}

	if (block_size < frozen_size) {
		pr_err("utag size %zu too big\n", frozen_size);
		return NULL;
	}

	ptr = buf = vmalloc(frozen_size);
	if (!buf)
		return NULL;

	cur = tags;
	/* root utag stores size of entire image in util word */
	cur->util = frozen_size;
	while (1) {
		written = 0;
		memcpy(frozen.name, cur->name, MAX_UTAG_NAME);
		frozen.flags = htonl(cur->flags);
		frozen.size = htonl(cur->size);
		frozen.util = htonl(cur->util);

		memcpy(ptr, &frozen, UTAG_MIN_TAG_SIZE);
		ptr += UTAG_MIN_TAG_SIZE;
		written += UTAG_MIN_TAG_SIZE;

		if (cur->size) {
			memcpy(ptr, cur->payload, cur->size);
			ptr += cur->size;
			written += cur->size;
		}

		/* pad with zeros if needed */
		zeros = ROUNDUP(cur->size, 4) - cur->size;
		if (zeros) {
			memset(ptr, 0, zeros);
			ptr += zeros;
			written += zeros;
		}

		pr_debug("written %zu bytes for utag [%s]\n",
			written, cur->name);

		if (!strncmp(cur->name, UTAG_TAIL, MAX_UTAG_NAME))
			break;

		cur = cur->next;
	}

	memset(ptr, 0, buf + frozen_size - ptr);
	if ((buf + frozen_size - ptr))
		pr_debug("padded %zu bytes\n", buf + frozen_size - ptr);
	if (tags_size)
		*tags_size = frozen_size;

	return buf;
}

/*
 * Try to load utags into memory from a partition on secondary storage.
 *
 * Not thread safe, call from a safe context only
 */
static struct utag *load_utags(struct blkdev *cb)
{
	size_t bytes;
	int ret_bytes;
	void *data;
	struct utag *head = NULL;
	struct ctrl *ctrl = container_of(cb, struct ctrl, main);

	if (open_utags(cb))
		return NULL;

	bytes = data_size(cb);

	/*
	 * make sure the block is at least big enough to hold header
	 * and footer, create empty partition for hwtags only
	 */
	if (UTAG_MIN_TAG_SIZE * 2 > bytes) {
		pr_err("[%s] invalid tags size %zu\n", ctrl->dir_name, bytes);
		if (!ctrl->hwtag)
			return NULL;
		if (init_empty(ctrl))
			return NULL;
		bytes = UTAG_MIN_TAG_SIZE * 2;
	}

	data = vmalloc(bytes);
	if (!data)
		return NULL;

	ret_bytes = kernel_read_stub(cb, data, bytes);
	if (bytes != ret_bytes) {
		pr_err("(%s) read failed ret %d\n", cb->name, ret_bytes);
		goto free_data;
	}

	head = thaw_tags(bytes, data);
	if (!head && ctrl->hwtag)
		init_empty(ctrl);

	/* Save pointer to the root attributes UTAG if present */
	if (ctrl->hwtag) {
		ctrl->attrib = find_first_utag(head, ".attributes");
		pr_debug(" .attributes %s\n", ctrl->attrib ?
			"found" : "not found");
		ctrl->features = find_first_utag(head, ".features");
		pr_debug(" .features %s\n", ctrl->features ?
			 "found" : "not found");
	}

 free_data:
	vfree(data);
	return head;
}

/*
 * Wrapper to call load_utags as a work function
 *
 */
void load_work_func(struct work_struct *work)
{
	struct ctrl *ctrl = container_of(work, struct ctrl, load_work);

	ctrl->head = load_utags(&ctrl->main);
	complete(&ctrl->load_comp);
}

static int full_utag_name(struct proc_node *pnode, char *tag)
{
	int i, subdir, blen;
	char *subdir_names[UTAG_DEPTH];
	struct ctrl *ctrl = pnode->ctrl;
	struct proc_dir_entry *parent;
	struct dir_node *c, *dir_node;

	*tag = 0;
	for (subdir = 0, parent = pnode->dir; parent;) {
		dir_node = NULL;
		list_for_each_entry(c, &ctrl->dir_list, entry) {
			if (c->dir == parent) {
				dir_node = c;
				break;
			}
		}

		if (!dir_node)
			break;

		pr_debug("dir [%s] has %sparent\n", dir_node->name,
			dir_node->parent == ctrl->root ? "no " : "");

		subdir_names[subdir++] = dir_node->name;
		parent = dir_node->parent;
	}

	pr_debug("utag consists of %d subdirs\n", subdir);

	/* apply subdirs in opposite order */
	for (blen = 0, i = subdir - 1; i >= 0; i--)
		blen += scnprintf(tag + blen, MAX_UTAG_NAME - blen,
			"%s%s", subdir_names[i], i ? "/" : "");

	if (pnode->type[0] != 0)
		/* top off with type strings */
		blen += scnprintf(tag + blen, MAX_UTAG_NAME - blen,
			":%s", pnode->type);

	pr_debug("full name [%s](%d) has %d subdirs\n", tag, blen, subdir);

	return blen;
}

static int check_utag_range(char *tag, struct utag *head, char *data,
	size_t count)
{
	char rtag[MAX_UTAG_NAME];
	struct utag *range;
	char *buf, *ptr, *tok;
	size_t check;
	bool found = false;
	size_t len, data_len;

	/* copy utag name and append .range */
	strlcpy(rtag, tag, MAX_UTAG_NAME);
	tok = strnchr(rtag, MAX_UTAG_NAME, ':');
	if (tok)
		*tok = 0;
	if (strlcat(rtag, "/.range", MAX_UTAG_NAME) > MAX_UTAG_NAME) {
		pr_err("full name [%s] .range error\n", rtag);
		return -EIO;
	}

	pr_debug("utag range check [%s]\n", rtag);

	/* walk the thawed list to find the range */
	range = find_first_utag(head, rtag);
	if (!range) {
		pr_debug("full name [%s] no .range\n", rtag);
		return 0;
	}

	if (!range->size) {
		pr_debug("full name [%s] .range not set\n", rtag);
		return 0;
	}

	/* check sizes and termination of the data */
	check = strnlen(range->payload, range->size);
	if (check == range->size) {
		pr_err("range payload for [%s] is not null terminated\n", rtag);
		return -EIO;
	}

	if (count > (check+1)) {
		pr_err("data for [%s] is not allowed %zu > %zu\n", tag, count, check+1);
		return -EIO;
	}

	data_len = strnlen(data, count);
	if (count == data_len) {
		pr_err("data for [%s] not a string\n", tag);
		return -EIO;
	}

	/* make local copy of .range payload to tokenize */
	buf = ptr = kzalloc(range->size + 1, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	memcpy(buf, range->payload, range->size);

	while (ptr && !found) {
		tok = strsep(&ptr, ",");
		len = strnlen(tok, MAX_UTAG_SIZE);
		pr_debug("range value [%s] for [%s]\n", tok, rtag);
		/* we can skip right away if length does not match */
		if (data_len != len)
			continue;
		if (!strncmp(tok, data, len))
			found = true;
	}
	kfree(buf);

	/* if ptr is still NULL we did not find a match */
	if (!found) {
		pr_err("data [%s] out of range @[%s]\n", data, rtag);
		return -EINVAL;
	}
	pr_debug("data [%s] in range\n", data);

	return 0;
}

static int replace_first_utag(struct utag *head, char *name,
		void *payload, size_t size)
{
	struct utag *utag;
	void *oldpayload;

	/* search for the first occurrence of specified type of tag */
	utag = find_first_utag(head, name);
	if (!utag)
		return 0;

	oldpayload = utag->payload;
	if (utag->flags & UTAG_FLAG_PROTECTED) {
		pr_err("protected utag %s\n", name);
		return -EIO;
	}

	utag->payload = kzalloc(size + 1, GFP_KERNEL);
	if (!utag->payload) {
		utag->payload = oldpayload;
		return -EIO;
	}

	memcpy(utag->payload, payload, size);
	utag->size = size;
	kfree(oldpayload);
	return 0;
}

static int store_utags(struct ctrl *ctrl, struct utag *tags)
{
	size_t written;
	size_t tags_size;
	char *datap = NULL;
	int rc = 0;
	struct blkdev *cb = &ctrl->main;

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 15, 0)
	mm_segment_t fs;
	fs = get_fs();
	set_fs(KERNEL_DS);
#endif

	pr_debug("[%s] utags partition blk_sz=%zu\n", ctrl->dir_name, cb->size);

	datap = freeze_tags(cb->size, tags, &tags_size);
	if (!datap) {
		rc = -EIO;
		goto out;
	}

	if (open_utags(cb)) {
		rc = -EIO;
		goto err_free;
	}

	written = kernel_write_stub(cb, datap, tags_size);
	if (written < tags_size) {
		pr_err("failed to write file (%s), rc=%zu\n",
			cb->name, written);
		rc = -EIO;
	}

	/* Only try to use backup partition if it is configured */
	if (ctrl->backup.name) {
		cb = &ctrl->backup;
		if (open_utags(cb)) {
			rc = -EIO;
			goto err_free;
		}

		written = kernel_write_stub(cb, datap, tags_size);
		if (written < tags_size) {
			pr_err("failed to write file (%s), rc=%zu\n",
				cb->name, written);
			rc = -EIO;
		}
	}

err_free:
	vfree(datap);
out:
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 15, 0)
	set_fs(fs);
#endif
	return rc;
}

/*
 * Wrappers to call store_utags as a work function
 *
 */
void store_work_func(struct work_struct *work)
{
	int rc;
	struct ctrl *ctrl = container_of(work, struct ctrl, store_work);

	rc = store_utags(ctrl, ctrl->head);
	if (rc)
		pr_err("error storing utags partition\n");
	ctrl->store_work_result = rc;
	complete(&ctrl->store_comp);
}

static int read_utag(struct seq_file *file, void *v)
{
	int i, error;
	char utag_name[MAX_UTAG_NAME];
	uint8_t *ptr;
	struct utag *tags = NULL;
	struct utag *tag = NULL;
	struct proc_node *proc = (struct proc_node *)file->private;
	struct ctrl *ctrl = proc->ctrl;
	int rc = 0;

	mutex_lock(&ctrl->access_lock);
	queue_work(ctrl->load_queue, &ctrl->load_work);
	wait_for_completion(&ctrl->load_comp);
	tags = ctrl->head;
	if (NULL == tags) {
		pr_err("load utags error\n");
		mutex_unlock(&ctrl->access_lock);
		return -EFAULT;
	}

	/* traverse back all parent directories up to root */
	error = full_utag_name(proc, utag_name);
	if (!error) {
		seq_puts(file, "cannot find utag associated with this file\n");
		rc = -EINVAL;
		goto free_tags_exit;
	}

	tag = find_first_utag(tags, utag_name);
	if (NULL == tag) {
		seq_printf(file, "utag [%s] not found\n", utag_name);
		rc = -EINVAL;
		goto free_tags_exit;
	}

	switch (proc->mode) {
	case OUT_ASCII:
		seq_printf(file, "%s", (char *)tag->payload);
		break;
	case OUT_RAW:
		ptr = (uint8_t *) tag->payload;
		for (i = 0; i < tag->size; i++)
			seq_printf(file, "%02X", *(ptr + i));
		break;
	case OUT_TYPE:
		if (*(char *)proc->type != 0)
			seq_printf(file, "%s", (char *)proc->type);
		break;
	}
	seq_puts(file, "\n");

free_tags_exit:
	free_tags(tags);
	mutex_unlock(&ctrl->access_lock);
	return rc;
}

static ssize_t write_utag(struct file *file, const char __user *buffer,
	   size_t count, loff_t *pos)
{
	int i, error;
	char *payload, utag[MAX_UTAG_NAME];
	struct utag *tags = NULL;
	struct inode *inode = file_inode(file);
	struct proc_node *proc = PDE_DATA(inode);
	struct ctrl *ctrl = proc->ctrl;
	size_t lco, length = count;

	pr_debug("%zu bytes write attempt to [%s](%d)\n",
					count, proc->name, proc->mode);
	if (OUT_TYPE == proc->mode) {
		return -EINVAL;
	}

	if (MAX_UTAG_SIZE < count) {
		pr_err("error utag too big %zu\n", count);
		return -EINVAL;
	}
	/* allocate an extra byte in case we need to add null-byte */
	payload = kzalloc(count + 1, GFP_KERNEL);
	if (!payload)
		return -ENOMEM;

	if (copy_from_user(payload, buffer, count)) {
		pr_err("user copy error\n");
		count = -EIO;
		goto free_temp_exit;
	}

	lco = count - 1; /* last character offset */
	if (payload[lco] == '\n') {
		payload[lco] = 0;
		/* update offset only if input has anything else, but '\n' */
		if (lco)
			lco--;
		length--;
		pr_debug("replace trailing newline with null-byte\n");
	}
	/* make sure ASCII input is properly terminated */
	if (proc->mode == OUT_ASCII) {
		for (i = 0; i <= lco; i++)
			if (payload[i] == 0)
				break;
		if (i > lco) {
			payload[++lco] = 0;
			length++;
			pr_debug("add trailing null-byte\n");
		}
	}

	mutex_lock(&ctrl->access_lock);
	queue_work(ctrl->load_queue, &ctrl->load_work);
	wait_for_completion(&ctrl->load_comp);
	tags = ctrl->head;
	if (NULL == tags) {
		pr_err("[%s] load error\n", ctrl->dir_name);
		count = -EIO;
		goto free_temp_exit;
	}

	if (ctrl->lock) {
		pr_err("[%s] [%s] is locked\n", proc->name, ctrl->dir_name);
		count = -EACCES;
		goto free_tags_exit;
	}

	/* traverse back all parent directories up to root */
	error = full_utag_name(proc, utag);
	if (!error) {
		pr_err("cannot find utag associated with this file\n");
		count = -EIO;
		goto free_tags_exit;
	}

	/* check if this utag has .range child only for hwtags */
	if (ctrl->hwtag && length) {
		error = check_utag_range(utag, tags, payload, length);
		if (error) {
			count = -EINVAL;
			goto free_tags_exit;
		}
	}

	error = replace_first_utag(tags, utag, payload, length);
	if (error) {
		pr_err("error storing [%s] new payload\n", utag);
		count = -EIO;
		goto free_tags_exit;
	}

	queue_work(ctrl->store_queue, &ctrl->store_work);
	wait_for_completion(&ctrl->store_comp);
	if (ctrl->store_work_result)
		count = ctrl->store_work_result;
free_tags_exit:
	free_tags(tags);
free_temp_exit:
	kfree(payload);
	mutex_unlock(&ctrl->access_lock);
	return count;
}

static int rebuild_utags_directory(struct ctrl *ctrl)
{
	clear_utags_directory(ctrl);
	return build_utags_directory(ctrl);
}

/*
 * Process delete file request. Check for existing utag,
 * delete it, save utags, update proc fs
*/

static ssize_t delete_utag(struct file *file, const char __user *buffer,
	   size_t count, loff_t *pos)
{
	char *pattern, expendable[MAX_UTAG_NAME];
	struct utag *tags, *cur, *next;
	struct inode *inode = file_inode(file);
	struct ctrl *ctrl = PDE_DATA(inode);

	if ((MAX_UTAG_NAME < count) || (0 == count)) {
		pr_err("invalid utag name %zu\n", count);
		return -EIO;
	}

	if (copy_from_user(expendable, buffer, count)) {
		pr_err("user copy error\n");
		return -EFAULT;
	}

	/* payload has input string plus \n. Replace \n with \0 */
	expendable[count-1] = 0;
	if (!validate_name(expendable, count-1)) {
		pr_err("invalid format %s\n", expendable);
		return -EINVAL;
	}

	if (check_path(expendable, count-1))
		return -EINVAL;

	mutex_lock(&ctrl->access_lock);
	queue_work(ctrl->load_queue, &ctrl->load_work);
	wait_for_completion(&ctrl->load_comp);
	tags = ctrl->head;
	if (NULL == tags) {
		pr_err("[%s] load error\n", ctrl->dir_name);
		mutex_unlock(&ctrl->access_lock);
		return -EINVAL;
	}

	if (ctrl->lock) {
		pr_err("[%s] [%s] is locked\n", expendable, ctrl->dir_name);
		count = -EACCES;
		goto just_leave;
	}

	cur = find_first_utag(tags, expendable);
	if (!cur) {
		pr_err("cannot find utag %s\n", expendable);
		count = -EINVAL;
		goto just_leave;
	}

	/* update pointers */
	cur->prev->next = cur->next;
	cur->next->prev = cur->prev;
	kfree(cur);
	pr_debug("deleted utag [%s]\n", expendable);

	/* remove all utags beneath */
	for (cur = tags->next; cur->next;) {
		pattern = strnstr(cur->name, expendable, MAX_UTAG_NAME);
		/* any subutags will start with the suffix followed by a '/' */
		if ((pattern == cur->name) && (cur->name[count-1] == '/')) {
			pr_debug("deleting utag [%s]\n", cur->name);
			next = cur->next;
			cur->prev->next = cur->next;
			cur->next->prev = cur->prev;
			kfree(cur);
			cur = next;
			continue;
		}
		cur = cur->next;
	}

	/* Store changed partition */
	queue_work(ctrl->store_queue, &ctrl->store_work);
	wait_for_completion(&ctrl->store_comp);
	if (ctrl->store_work_result)
		count = ctrl->store_work_result;
	rebuild_utags_directory(ctrl);
just_leave:
	free_tags(tags);
	mutex_unlock(&ctrl->access_lock);
	return count;
}

static int lock_show(struct seq_file *file, void *v)
{
	struct ctrl *ctrl = (struct ctrl *)file->private;

	if (!ctrl) {
		pr_err("no control data set\n");
		return -EIO;
	}

	seq_printf(file, "%u\n", ctrl->lock);
	return 0;
}

/* Check utag  name againts valid range saved in vld utag  payload */

static int check_hwtag(struct ctrl *ctrl, char *attr, struct utag *vld)
{

char *buf, *ptr, *tok;
bool found = false;
size_t len, alen;

	if (!vld || !attr) {
		pr_debug("no validation tag or data\n");
		return 0;
	}

	if (!vld->size) {
		pr_err("allow [%s] because [%s] is empty\n", attr, vld->name);
		return 0;
	}

	alen = strnlen(attr, MAX_UTAG_NAME);
	pr_debug("name check for [%s] length [%zu]\n", attr, alen);
	/* make local copy of validation payload to tokenize */
	buf = ptr = kzalloc(vld->size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	memcpy(ptr, vld->payload, vld->size);

	while (ptr && !found) {
		tok = strsep(&ptr, ",");
		/* *ptr is updated to show past the token */
		len = strnlen(tok, MAX_UTAG_NAME);
		if (alen > len)
			continue;
		pr_debug("attr value [%s] len [%zu]\n", tok, len);
		if (!strncmp(tok, attr, len))
			found = true;
	}
	kfree(buf);

	/* if ptr is still NULL we did not find a match */
	if (!found) {
		pr_err("[%s] is not allowed\n", attr);
		return -EINVAL;
	}

	return 0;
}

/*
 * Process new file request. Check for existing utag,
 * add empty new utag, save utags and add file interface
*/

static ssize_t new_utag(struct file *file, const char __user *buffer,
	   size_t count, loff_t *pos)
{
	struct inode *inode = file_inode(file);
	struct ctrl *ctrl = PDE_DATA(inode);
	struct utag *tags, *cur;
	struct proc_dir_entry *parent = NULL;
	char tree[UTAG_DEPTH][MAX_UTAG_NAME];
	char expendable[MAX_UTAG_NAME], *names[UTAG_DEPTH], *type = NULL;
	int error, i, num_names;
	size_t ret = count;

	if ((MAX_UTAG_NAME < count) || (0 == count)) {
		pr_err("invalid utag name %zu\n", count);
		return -EIO;
	}

	if (copy_from_user(expendable, buffer, count)) {
		pr_err("user copy error\n");
		return -EFAULT;
	}
	/* payload has input string plus \n. Replace \n with \0 */
	expendable[count-1] = 0;
	if (!validate_name(expendable, count-1)) {
		pr_err("invalid format %s\n", expendable);
		return -EFAULT;
	}

	if (check_path(expendable, count-1))
		return -EFAULT;

	if (no_show_tag(expendable)) {
		pr_err("[%s] reserved name %s\n", ctrl->dir_name, expendable);
		return -EFAULT;
	}

	pr_debug("adding [%s] utag\n", expendable);

	mutex_lock(&ctrl->access_lock);
	queue_work(ctrl->load_queue, &ctrl->load_work);
	wait_for_completion(&ctrl->load_comp);
	tags = ctrl->head;
	if (NULL == tags) {
		pr_err("[%s] load error\n", ctrl->dir_name);
		mutex_unlock(&ctrl->access_lock);
		return -EFAULT;
	}

	if (ctrl->lock) {
		pr_err("[%s] [%s] is locked\n", expendable, ctrl->dir_name);
		ret = -EACCES;
		goto just_leave;
	}

	/* Ignore request if utag name already in use */
	cur = find_first_utag(tags, expendable);
	if (NULL != cur) {
		pr_err("cannot create [%s]; already in use\n", expendable);
		ret = -EINVAL;
		goto just_leave;
	}


	num_names = full_split(expendable, names, &type);
	if (num_names == 0) {
		pr_err("failed to split path\n");
		ret = -EINVAL;
		goto just_leave;
	}

	/* check if we are trying to add an attribute file */
	/* check every level of the path for validity */
	for (i = 0; i < num_names; i++) {
		/* build name for each sublevel and add */
		/* slash to all sublevels except first */

		scnprintf(tree[i], MAX_UTAG_NAME, "%s%s%s", i ? tree[i-1] : "",
			i ? "/" : "", names[i]);
		pr_debug("prepared subdir [%s] as [%s]\n", names[i], tree[i]);

		/* only check features for files that do not have a .*/
		if (ctrl->hwtag && '.' != names[i][0])
			if (check_hwtag(ctrl, tree[i], ctrl->features)) {
				ret = -EINVAL;
				goto just_leave;
			}
	}

	/* only check attributes for last file starting with a . */
	/* do not check for attributes in the root */
	if (ctrl->hwtag && num_names != 1 && '.' == names[num_names-1][0])
		if (check_hwtag(ctrl, names[num_names-1], ctrl->attrib)) {
			ret = -EINVAL;
			goto just_leave;
		}

	for (i = 0; i < num_names; i++) {
		error = add_utag_tail(tags, tree[i], type);
		if (error == -EEXIST) {
			struct dir_node *dnode;
			/* need to update parent to ensure proper hierarchy */
			dnode = find_dir_node(ctrl, tree[i]);
			parent = dnode->dir;
			continue;
		}

		/* create utag dir for every part of utag name */
		parent = proc_utag_dir(ctrl, names[i], tree[i], type,
			true, parent);
		if (IS_ERR(parent))
			break;
	}

	walk_dir_nodes(ctrl);
	walk_proc_nodes(ctrl);

	/* Store changed partition */
	queue_work(ctrl->store_queue, &ctrl->store_work);
	wait_for_completion(&ctrl->store_comp);
	if (ctrl->store_work_result)
		ret = ctrl->store_work_result;
just_leave:
	free_tags(tags);
	mutex_unlock(&ctrl->access_lock);
	return ret;
}

static int reload_show(struct seq_file *file, void *v)
{
	struct ctrl *ctrl = (struct ctrl *)file->private;

	if (!ctrl)
		pr_err("no control data set\n");
	else
		seq_printf(file, "%c\n", ctrl->reload);

	pr_debug("[%s] %c\n", ctrl->dir_name, ctrl->reload);
	return 0;
}

static ssize_t reload_write(struct file *file, const char __user *buffer,
	   size_t count, loff_t *pos)
{
	struct inode *inode = file_inode(file);
	struct ctrl *ctrl = PDE_DATA(inode);


	/* only single character input plus new line */
	if (2 < count) {
		pr_err("invalid command length\n");
		return -EIO;
	}

	mutex_lock(&ctrl->access_lock);

	if (UTAG_STATUS_LOADED == ctrl->reload) {
		pr_info("[%s] (pid %i) [%s] already loaded\n",
			current->comm, current->pid, ctrl->dir_name);
		mutex_unlock(&ctrl->access_lock);
		return count;
	}

	if (copy_from_user(&ctrl->reload, buffer, 1)) {
		pr_err("user copy error\n");
		mutex_unlock(&ctrl->access_lock);
		return -EFAULT;
	}

	pr_info("[%s] (pid %i) [%s] %c\n",
		current->comm, current->pid, ctrl->dir_name, ctrl->reload);

	if (UTAG_STATUS_RELOAD == ctrl->reload) {
		if (rebuild_utags_directory(ctrl))
			ctrl->reload = UTAG_STATUS_FAILED;
	}

	mutex_unlock(&ctrl->access_lock);
	return count;
}

static int lock_open(struct inode *inode, struct file *file)
{
	return single_open(file, lock_show, PDE_DATA(inode));
}

static int reload_open(struct inode *inode, struct file *file)
{
	return single_open(file, reload_show, PDE_DATA(inode));
}

static int partition_open(struct inode *inode, struct file *file)
{
	return single_open(file, read_utag, PDE_DATA(inode));
}

#if KERNEL_VERSION(5, 10, 0) <= LINUX_VERSION_CODE
static const struct proc_ops reload_fops = {
	.proc_open = reload_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
	.proc_write = reload_write,
};
#else
static const struct file_operations reload_fops = {
	.owner = THIS_MODULE,
	.open = reload_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
	.write = reload_write,
};
#endif

static int build_utags_directory(struct ctrl *ctrl)
{
	struct proc_dir_entry *parent;
	struct utag *tags, *cur;
	int rc = 0;

	/* try to load utags from primary partition */
	queue_work(ctrl->load_queue, &ctrl->load_work);
	wait_for_completion(&ctrl->load_comp);
	tags = ctrl->head;
	if (NULL == tags) {
		pr_err("[%s] load error\n", ctrl->dir_name);
		return -EIO;
	}
	/* skip utags head */
	cur = tags->next;
	while (1) {
		int i, num_names;
		char expendable[MAX_UTAG_NAME];
		char *type, *names[UTAG_DEPTH];
		char path[MAX_UTAG_NAME];

		memset(path, 0, MAX_UTAG_NAME);

		/* hook to skip special tags if we have any*/
		if (no_show_tag(cur->name)) {
			cur = cur->next;
			continue;
		}

		/* skip utags tail */
		if (cur->next == NULL)
			break;

		memcpy(expendable, cur->name, MAX_UTAG_NAME);
		parent = NULL, type = NULL;
		num_names = full_split(expendable, names, &type);

		for (i = 0; i < num_names; i++) {
			scnprintf(path + strlen(path),
				MAX_UTAG_NAME - strlen(path),
				"%s%s", i ? "/" : "", names[i]);
			pr_debug("creating subdir %s as %s\n", names[i], path);
			parent = proc_utag_dir(ctrl, names[i], path, type,
					/* populate only utag dir */
					(i == (num_names - 1)) ? true : false,
					parent);
			if (IS_ERR(parent)) {
				rc = -EINVAL;
				goto stop_building_utags;
			}
		}

		cur = cur->next;
	}

stop_building_utags:

	walk_dir_nodes(ctrl);
	walk_proc_nodes(ctrl);

	free_tags(tags);
	if (!rc)
		ctrl->reload = UTAG_STATUS_LOADED;
	return rc;
}

#ifdef CONFIG_OF
static char *bootargs_str;

static int utag_get_bootarg(char *key, char **value, char *prop, char *spl_flag)
{
	const char *bootargs_ptr = NULL;
	char *idx = NULL;
	char *kvpair = NULL;
	int err = 1;
	struct device_node *n = of_find_node_by_path("/chosen");
	size_t bootargs_ptr_len = 0;

	if (n == NULL)
		goto err;

	if (of_property_read_string(n, prop, &bootargs_ptr) != 0)
		goto err_putnode;

	bootargs_ptr_len = strlen(bootargs_ptr);
	if (!bootargs_str) {
		/* Following operations need a non-const version of bootargs */
		bootargs_str = kzalloc(bootargs_ptr_len + 1, GFP_KERNEL);
		if (!bootargs_str)
			goto err_putnode;
	}
	strlcpy(bootargs_str, bootargs_ptr, bootargs_ptr_len + 1);

	idx = strnstr(bootargs_str, key, strlen(bootargs_str));
	if (idx) {
		kvpair = strsep(&idx, " ");
		if (kvpair)
			if (strsep(&kvpair, "=")) {
				*value = strsep(&kvpair, spl_flag);
				if (*value)
					err = 0;
			}
	}

err_putnode:
	of_node_put(n);
err:
	return err;
}

#ifdef CONFIG_BOOT_CONFIG
static char bootdevice_name[256];
static int utags_get_bootdevice_from_bootconfig(char **bootconfig_val)
{
	static char bootdevice_init = 0;
	int rc = 0;

	if (bootdevice_init) {
		*bootconfig_val = bootdevice_name;
		return 0;
	}

	rc = utag_get_bootarg("androidboot.bootdevice=", bootconfig_val, "mmi,bootconfig", "\n");
	if (!rc && *bootconfig_val) {
		strncpy(bootdevice_name, *bootconfig_val, strlen(*bootconfig_val));
		bootdevice_init = 1;
	}
	return rc;
}
#endif

#define PLATFORM_PATH "/dev/block/platform/soc/"

static void utags_bootdevice_expand(const char **name_ptr, const char *name)
{
	int rc = -1;
	size_t max_len = strlen(name);
	char *bootdevice = NULL;
	char *replace, *suffix, *expanded;

	if (name == NULL)
		return;
#ifndef CONFIG_BOOT_CONFIG
	rc = utag_get_bootarg("androidboot.bootdevice=", &bootdevice, "bootargs", " ");
	if (rc || !bootdevice)
		goto need_no_expansion;
#else
	rc = utags_get_bootdevice_from_bootconfig(&bootdevice);

	if (rc || !bootdevice) {
		rc = utag_get_bootarg("androidboot.bootdevice=", &bootdevice, "bootargs", " ");
		if (rc || !bootdevice)
			goto need_no_expansion;
	}
#endif

	replace = strnstr(name, "bootdevice", max_len);
	suffix = strnstr(name, "by-name", max_len);
	if (!replace || !suffix)
		goto need_no_expansion;

	max_len = strlen(PLATFORM_PATH) + strlen(bootdevice) + strlen(suffix);
	max_len += 2;	/* account on '/' and null termination */

	expanded = kzalloc(max_len, GFP_KERNEL);
	if (!expanded) {
		pr_err("cannot expand bootdevice\n");
		goto need_no_expansion;
	}

	snprintf(expanded, max_len,
			PLATFORM_PATH "%s/%s", bootdevice, suffix);
	pr_info("path expanded to: %s(%zu)\n", expanded, max_len);
	*name_ptr = (const char *)expanded;

	return;

need_no_expansion:
	*name_ptr = kstrdup(name, GFP_KERNEL);
}

static int utags_dt_init(struct platform_device *pdev)
{
	int rc;
	const char *path_ptr;
	struct device_node *node = pdev->dev.of_node;
	struct ctrl *ctrl;

	ctrl = dev_get_drvdata(&pdev->dev);
	rc = of_property_read_string(node, "mmi,main-utags", &path_ptr);
	if (rc) {
		pr_err("storage path not provided\n");
		return -EIO;
	}
	utags_bootdevice_expand(&ctrl->main.name, path_ptr);

	rc = of_property_read_string(node, "mmi,backup-utags", &path_ptr);
	if (rc)
		pr_info("backup storage path not provided\n");
	utags_bootdevice_expand(&ctrl->backup.name, path_ptr);

	ctrl->dir_name = DEFAULT_ROOT;
	rc = of_property_read_string(node, "mmi,dir-name", &ctrl->dir_name);
	if (!rc)
		pr_debug("utag dir override %s\n", ctrl->dir_name);

	return 0;
}
#else
static int utags_dt_init(struct platform_device *pdev) { return -EINVAL; }
#endif

static void clear_utags_directory(struct ctrl *ctrl)
{
	struct proc_node *node, *s = NULL;
	struct dir_node *dir_node, *c = NULL;

	list_for_each_entry_safe(dir_node, c, &ctrl->dir_list, entry) {
		if (dir_node->parent != ctrl->root)
			continue;
		/* remove whole subtree of first level subdir */
		remove_proc_subtree(dir_node->name, ctrl->root);

		pr_debug("removing subtree [%s]\n", dir_node->name);

		list_del(&dir_node->entry);
		kfree(dir_node);
	}

	/* all subtrees removed; just free memory */
	list_for_each_entry_safe(dir_node, c, &ctrl->dir_list, entry) {
		list_del(&dir_node->entry);
		kfree(dir_node);
	}

	/* nothing left in procfs except reload file; free memory */
	list_for_each_entry_safe(node, s, &ctrl->node_list, entry) {
		list_del(&node->entry);
		kfree(node);
	}
}


#define UTAGS_QNAME_SIZE 16
static int utags_probe(struct platform_device *pdev)
{
	int rc;
	struct ctrl *ctrl;
	char buf[UTAGS_QNAME_SIZE];

	ctrl = devm_kzalloc(&pdev->dev, sizeof(struct ctrl), GFP_KERNEL);
	if (!ctrl)
		return -ENOMEM;

	INIT_LIST_HEAD(&ctrl->dir_list);
	INIT_LIST_HEAD(&ctrl->node_list);
	ctrl->pdev = pdev;
	ctrl->reload = UTAG_STATUS_NOT_READY;
	mutex_init(&ctrl->access_lock);

	init_completion(&ctrl->load_comp);
	init_completion(&ctrl->store_comp);
	INIT_WORK(&ctrl->load_work, load_work_func);
	INIT_WORK(&ctrl->store_work, store_work_func);

	dev_set_drvdata(&pdev->dev, ctrl);

	rc = utags_dt_init(pdev);
	if (rc)
		return -EIO;

	scnprintf(buf, UTAGS_QNAME_SIZE, "%s_load", ctrl->dir_name);
	ctrl->load_queue = create_workqueue(buf);
	if (!ctrl->load_queue) {
		pr_err("Failed to create workqueue %s\n", buf);
		return -ENOMEM;
	}

	scnprintf(buf, UTAGS_QNAME_SIZE, "%s_store", ctrl->dir_name);
	ctrl->store_queue = create_workqueue(buf);
	if (!ctrl->store_queue) {
		destroy_workqueue(ctrl->load_queue);
		pr_err("Failed to create workqueue %s\n", buf);
		return -ENOMEM;
	}

	ctrl->root = proc_mkdir_data(ctrl->dir_name, 0771, NULL, NULL);
	if (!ctrl->root) {
		destroy_workqueue(ctrl->load_queue);
		destroy_workqueue(ctrl->store_queue);
		pr_err("Failed to create dir entry\n");
		return -EIO;
	}

	if (!strncmp(ctrl->dir_name, HW_ROOT, sizeof(HW_ROOT)))
		ctrl->hwtag = 1;

	if (!proc_create_data("reload", 0660, ctrl->root, &reload_fops, ctrl)) {
		pr_err("Failed to create reload entry\n");
		destroy_workqueue(ctrl->load_queue);
		destroy_workqueue(ctrl->store_queue);
		remove_proc_subtree(ctrl->dir_name, NULL);
		return -EIO;
	}

	if (proc_utag_util(ctrl)) {
		pr_err("Failed to create util dir\n");
		destroy_workqueue(ctrl->load_queue);
		destroy_workqueue(ctrl->store_queue);
		remove_proc_subtree(ctrl->dir_name, NULL);
		return -EFAULT;
	}

	pr_info("Done [%s]\n", ctrl->dir_name);
	return 0;
}

static int utags_remove(struct platform_device *pdev)
{
	struct ctrl *ctrl = dev_get_drvdata(&pdev->dev);

	clear_utags_directory(ctrl);
	remove_proc_subtree(ctrl->dir_name, NULL);
	destroy_workqueue(ctrl->load_queue);
	destroy_workqueue(ctrl->store_queue);
	if (ctrl->main.filep)
		filp_close(ctrl->main.filep, NULL);
	if (ctrl->backup.filep)
		filp_close(ctrl->backup.filep, NULL);

	if (ctrl->main.name)
		kfree(ctrl->main.name);
	if (ctrl->backup.name)
		kfree(ctrl->backup.name);

	if (bootargs_str) {
		kfree(bootargs_str);
		bootargs_str = NULL;
	}

	devm_kfree(&pdev->dev, ctrl);
	dev_set_drvdata(&pdev->dev, NULL);
	return 0;
}

#ifdef CONFIG_OF
static struct of_device_id utags_match_table[] = {
	{	.compatible = "mmi,utags",
	},
	{}
};
#endif

static struct platform_driver utags_driver = {
	.probe = utags_probe,
	.remove = utags_remove,
	.driver = {
		.name = DRVNAME,
		.bus = &platform_bus_type,
		.owner	= THIS_MODULE,
		.of_match_table = of_match_ptr(utags_match_table),
	},
};

static int __init utags_init(void)
{
	return platform_driver_register(&utags_driver);
}

static void __exit utags_exit(void)
{
	platform_driver_unregister(&utags_driver);
}

late_initcall(utags_init);
module_exit(utags_exit);

MODULE_LICENSE("GPL");
#if KERNEL_VERSION(5, 4, 0) <= LINUX_VERSION_CODE
MODULE_IMPORT_NS(VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver);
#endif
MODULE_AUTHOR("Motorola Mobility LLC");
MODULE_DESCRIPTION("Configuration module");
