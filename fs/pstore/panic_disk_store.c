/* SPDX-License-Identifier: GPL-2.0 */

#ifdef CONFIG_PANIC_DISK_STORE

#include <linux/atomic.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kmsg_dump.h>
#include <linux/mount.h>
#include <linux/namei.h>
#include <linux/time.h>
#include <linux/timekeeping.h>
#include <linux/uaccess.h>

#define PANIC_DISK_STORE_DIR "/data/panic"
#define PANIC_DISK_STORE_DATA "/data"
#define PANIC_DISK_STORE_BUFFER_SIZE (64 * 1024)
#define PANIC_DISK_STORE_DONE "\nLOG DONE\n"
#define PANIC_DISK_STORE_OPEN_RETRIES 4

static char panic_disk_store_buffer[PANIC_DISK_STORE_BUFFER_SIZE];
static atomic_t panic_disk_store_active = ATOMIC_INIT(0);

static bool panic_disk_store_data_mounted(const struct path *data)
{
	struct path root;
	bool mounted;

	/* Do not write to a rootfs /data directory before /data is mounted. */
	if (kern_path("/", LOOKUP_FOLLOW, &root))
		return false;

	mounted = data->mnt != root.mnt;
	path_put(&root);
	return mounted;
}

static int panic_disk_store_create_dir(void)
{
	struct path path;
	struct dentry *dentry;
	int ret;

	dentry = kern_path_create(AT_FDCWD, PANIC_DISK_STORE_DIR, &path, 0);
	if (IS_ERR(dentry))
		return PTR_ERR(dentry);

	ret = vfs_mkdir(d_inode(path.dentry), dentry, 0700);
	done_path_create(&path, dentry);
	return ret;
}

static int panic_disk_store_get_dir(struct path *data, struct path *panic_dir)
{
	int ret;

	ret = kern_path(PANIC_DISK_STORE_DATA,
			LOOKUP_FOLLOW | LOOKUP_DIRECTORY, data);
	if (ret)
		return ret;

	if (!panic_disk_store_data_mounted(data) ||
		sb_rdonly(data->dentry->d_sb) || __mnt_is_readonly(data->mnt)) {
		ret = -EROFS;
		goto out_data;
	}

	ret = kern_path(PANIC_DISK_STORE_DIR,
			LOOKUP_FOLLOW | LOOKUP_DIRECTORY, panic_dir);
	if (ret == -ENOENT) {
		ret = panic_disk_store_create_dir();
		if (ret)
			goto out_data;
		ret = kern_path(PANIC_DISK_STORE_DIR,
				LOOKUP_FOLLOW | LOOKUP_DIRECTORY, panic_dir);
	}
	if (ret)
		goto out_data;

	if (panic_dir->mnt != data->mnt) {
		ret = -EXDEV;
		path_put(panic_dir);
		goto out_data;
	}

	return 0;
out_data:
	path_put(data);
	return ret;
}

static struct file *panic_disk_store_open(void)
{
	struct path data;
	struct path panic_dir;
	struct file *file;
	char filename[sizeof(PANIC_DISK_STORE_DIR) + 40];
	struct tm tm;
	time64_t now;
	int ret;
	int attempt;

	ret = panic_disk_store_get_dir(&data, &panic_dir);
	if (ret)
		return ERR_PTR(ret);
	path_put(&panic_dir);
	path_put(&data);

	now = ktime_get_real_seconds();
	time64_to_tm(now, 0, &tm);
	for (attempt = 0; attempt < PANIC_DISK_STORE_OPEN_RETRIES; attempt++) {
		snprintf(filename, sizeof(filename), PANIC_DISK_STORE_DIR
			"/%04ld-%02d-%02d_%02d-%02d-%02d_%d.log",
			tm.tm_year + 1900L, tm.tm_mon + 1, tm.tm_mday,
			tm.tm_hour, tm.tm_min, tm.tm_sec, attempt);
		file = filp_open(filename,
			O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
		if (!IS_ERR(file) || PTR_ERR(file) != -EEXIST)
			return file;
	}

	return file;
}

static void panic_disk_store_dump(struct kmsg_dumper *dumper,
					  enum kmsg_dump_reason reason)
{
	struct file *file;
	loff_t pos = 0;
	ssize_t written;
	size_t len;

	if (reason != KMSG_DUMP_PANIC ||
		atomic_xchg(&panic_disk_store_active, 1))
		return;

	file = panic_disk_store_open();
	if (IS_ERR(file))
		return;

	while (kmsg_dump_get_buffer(dumper, true, panic_disk_store_buffer,
					   sizeof(panic_disk_store_buffer), &len)) {
		written = kernel_write(file, panic_disk_store_buffer, len, &pos);
		if (written != len)
			goto out_close;
	}

	kernel_write(file, PANIC_DISK_STORE_DONE,
		     sizeof(PANIC_DISK_STORE_DONE) - 1, &pos);
out_close:
	filp_close(file, NULL);
}

static struct kmsg_dumper panic_disk_store_dumper = {
	.dump = panic_disk_store_dump,
	.max_reason = KMSG_DUMP_PANIC,
};

static int __init panic_disk_store_init(void)
{
	return kmsg_dump_register(&panic_disk_store_dumper);
}
late_initcall(panic_disk_store_init);

#endif /* CONFIG_PANIC_DISK_STORE */
