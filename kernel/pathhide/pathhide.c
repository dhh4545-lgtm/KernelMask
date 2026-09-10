// SPDX-License-Identifier: GPL-2.0
/*
 * PathHide - simple selective path hiding LKM for Android arm64 / GKI 6.6.
 *
 * Simplified re-implementation modeled on the LKM-PathMask demo
 * (https://github.com/Andrea-lyz/LKM-PathMask). For controlled testing on
 * devices you own or administer only.
 *
 * The module reads a plain-text config file (one absolute path per line),
 * resolves every target to a (dev, inode) identity via kprobe-resolved
 * kern_path()/path_put() pointers, and then makes those paths look absent:
 *
 *   - __arm64_sys_getdents64 kretprobe filters target entries out of
 *     directory listings (matched by d_ino, so bind mounts and aliases
 *     are covered too).
 *   - inode_permission / vfs_getattr kretprobes turn direct stat-style
 *     checks on target inodes into -ENOENT.
 *   - __arm64_sys_{newfstatat,statx,faccessat,faccessat2,readlinkat,
 *     openat,openat2} kretprobes match the user path string by prefix
 *     and rewrite the syscall result to -ENOENT. openat/openat2 also
 *     close the fd the syscall body already allocated. The syscall
 *     entry stubs are used because ThinLTO on Android GKI inlines the
 *     VFS helpers into their callers, leaving the kallsyms entries
 *     alive but never actually called; the stubs sit at the very top
 *     of the syscall path and are always out-of-line.
 *
 * Hiding is global: every process on the system (root included) sees the
 * configured paths as nonexistent while the module is loaded.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/dcache.h>
#include <linux/err.h>
#include <linux/fcntl.h>
#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/path.h>
#include <linux/version.h>
#include <linux/dirent.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <asm/syscall.h>
#include <asm/unistd.h>

#define PH_LOG_PREFIX "pathhide: "

#define MAX_HIDE_TARGETS  64
#define TARGET_PATHS_LEN   4096
#define TARGET_TEXT_LEN   256
#define CONFIG_FILE_MAX   16384
#define GETDENTS_BUF_LIMIT 65536u

struct hidden_target {
	dev_t dev;
	unsigned long long ino;
	char path[TARGET_TEXT_LEN];
	/*
	 * False when the inode was not materialised at insmod time
	 * (i_ino == 0, happens on FUSE before the daemon fills the node).
	 * Matching by (ino=0) would hit every not-yet-materialised inode,
	 * so such targets rely on the path-based syscall hooks only.
	 */
	bool inode_ok;
};

static struct hidden_target targets[MAX_HIDE_TARGETS];
static unsigned int target_count;

/*
 * Preferred configuration input: a plain text file with one absolute
 * path per line. Blank lines and lines starting with '#' are ignored.
 * The KernelSU boot service passes /data/adb/pathhide/hide_paths.txt.
 */
static char *config_path;
module_param(config_path, charp, 0444);
MODULE_PARM_DESC(config_path,
	"Text config file, one absolute path to hide per line (blank lines and # comments ignored)");

/* Fallback for manual insmod testing: comma-separated absolute paths. */
static char target_paths[TARGET_PATHS_LEN];
module_param_string(target_paths, target_paths, sizeof(target_paths), 0444);
MODULE_PARM_DESC(target_paths,
	"Comma-separated absolute paths to hide (fallback when config_path is unset or unreadable)");

static bool hide_dirents = true;
module_param(hide_dirents, bool, 0444);
MODULE_PARM_DESC(hide_dirents,
	"Filter target entries out of getdents64 directory listings");

/* Read-only sysfs mirror for health checks from userspace. */
module_param_named(resolved_count, target_count, uint, 0444);
MODULE_PARM_DESC(resolved_count,
	"Number of target paths successfully resolved at load time");

/*
 * VFS helper resolution. kern_path()/path_put()/close_fd() may be pruned
 * from an OEM kernel's export table, so their addresses are resolved
 * through a throw-away kprobe registration instead of being imported
 * directly by the .ko.
 */
typedef int (*ph_kern_path_t)(const char *name, unsigned int flags,
			      struct path *path);
typedef void (*ph_path_put_t)(const struct path *path);
typedef int (*ph_close_fd_t)(unsigned int fd);
typedef struct file *(*ph_filp_open_t)(const char *filename, int flags,
				       umode_t mode);
typedef ssize_t (*ph_kernel_read_t)(struct file *file, void *buf,
				    size_t count, loff_t *pos);

static ph_kern_path_t ph_kern_path;
static ph_path_put_t ph_path_put;
static ph_close_fd_t ph_close_fd;
/*
 * filp_open()/kernel_read() are not importable by a generic module: the
 * former is often pruned from the export table, the latter lives in the
 * restricted VFS namespace. Both functions exist in the running kernel
 * though, so like kern_path() we resolve them at runtime through kprobe
 * instead of importing them directly.
 */
static ph_filp_open_t ph_filp_open;
static ph_kernel_read_t ph_kernel_read;

/*
 * On Android GKI builds with CONFIG_CFI_CLANG=y an indirect call through
 * a kprobe-resolved address fails the CFI type-id check (kprobe gives us
 * the raw function body, not the jump-table entry) and panics the kernel.
 * Wrap only these indirect call sites in __nocfi helpers.
 */
#ifndef __nocfi
#define __nocfi
#endif

static int __nocfi ph_invoke_kern_path(const char *name, unsigned int flags,
				       struct path *path)
{
	return ph_kern_path(name, flags, path);
}

static void __nocfi ph_invoke_path_put(const struct path *path)
{
	ph_path_put(path);
}

static int __nocfi ph_invoke_close_fd(unsigned int fd)
{
	if (!ph_close_fd)
		return -ENOSYS;
	return ph_close_fd(fd);
}

static struct file *__nocfi ph_invoke_filp_open(const char *filename,
						int flags, umode_t mode)
{
	return ph_filp_open(filename, flags, mode);
}

static ssize_t __nocfi ph_invoke_kernel_read(struct file *file, void *buf,
					     size_t count, loff_t *pos)
{
	return ph_kernel_read(file, buf, count, pos);
}

static unsigned long resolve_kernel_symbol_addr(const char *symbol_name)
{
	struct kprobe kp = {
		.symbol_name = symbol_name,
	};
	unsigned long addr;
	int ret;

	ret = register_kprobe(&kp);
	if (ret) {
		pr_warn(PH_LOG_PREFIX "resolve %s failed: %d\n",
			symbol_name, ret);
		return 0;
	}

	addr = (unsigned long)kp.addr;
	unregister_kprobe(&kp);

	if (!addr)
		pr_warn(PH_LOG_PREFIX "resolve %s returned NULL\n",
			symbol_name);

	return addr;
}

static int resolve_path_helpers(void)
{
	if (!ph_kern_path)
		ph_kern_path = (ph_kern_path_t)
			resolve_kernel_symbol_addr("kern_path");
	if (!ph_path_put)
		ph_path_put = (ph_path_put_t)
			resolve_kernel_symbol_addr("path_put");

	if (!ph_kern_path || !ph_path_put)
		return -ENOENT;

	/*
	 * close_fd() is needed by the openat exit hook to release the fd
	 * that the syscall already allocated before we override the return
	 * value to -ENOENT. It is optional: if the symbol is not
	 * resolvable we simply don't rewrite openat results.
	 */
	if (!ph_close_fd)
		ph_close_fd = (ph_close_fd_t)
			resolve_kernel_symbol_addr("close_fd");

	/*
	 * Config-file reads must also avoid direct symbol imports.
	 * filp_open()/kernel_read() are resolved at runtime so the module
	 * does not depend on their export status or VFS namespace.
	 */
	if (!ph_filp_open)
		ph_filp_open = (ph_filp_open_t)
			resolve_kernel_symbol_addr("filp_open");
	if (!ph_kernel_read)
		ph_kernel_read = (ph_kernel_read_t)
			resolve_kernel_symbol_addr("kernel_read");

	if (!ph_filp_open || !ph_kernel_read) {
		pr_warn(PH_LOG_PREFIX
			"could not resolve filp_open/kernel_read; config file will be unsupported\n");
	} else {
		pr_info(PH_LOG_PREFIX
			"resolved filp_open/kernel_read via kprobe\n");
	}

	pr_info(PH_LOG_PREFIX "resolved VFS path helpers via kprobe\n");
	return 0;
}

static inline bool is_target_inode(const struct inode *inode)
{
	unsigned int i;

	if (!inode || !inode->i_sb)
		return false;

	for (i = 0; i < target_count; i++) {
		if (!targets[i].inode_ok)
			continue;
		if (inode->i_ino == targets[i].ino &&
		    inode->i_sb->s_dev == targets[i].dev)
			return true;
	}

	return false;
}

static inline bool is_target_ino(__u64 ino)
{
	unsigned int i;

	for (i = 0; i < target_count; i++) {
		if (!targets[i].inode_ok)
			continue;
		if (ino == (__u64)targets[i].ino)
			return true;
	}

	return false;
}

/*
 * Path-string prefix match: p equals the target or lives under it
 * ("target/..." with a '/' boundary), so hiding /a also hides /a/b.
 */
static bool ph_path_prefix_match(const char *p, const struct hidden_target *t)
{
	size_t plen = strlen(t->path);

	if (!plen)
		return false;

	if (strncmp(p, t->path, plen) == 0) {
		char next = p[plen];

		if (!next || next == '/')
			return true;
	}

	return false;
}

static bool sys_path_matches_target(const char *p)
{
	unsigned int i;

	if (!p || p[0] != '/')
		return false;

	for (i = 0; i < target_count; i++) {
		if (ph_path_prefix_match(p, &targets[i]))
			return true;
	}

	return false;
}

static int add_target_path(const char *path_name)
{
	struct path path;
	struct inode *inode;
	int ret;

	if (target_count >= MAX_HIDE_TARGETS) {
		pr_warn(PH_LOG_PREFIX "too many targets, skip %s\n", path_name);
		return -ENOSPC;
	}

	if (!ph_kern_path || !ph_path_put)
		return -ENOENT;

	ret = ph_invoke_kern_path(path_name, LOOKUP_FOLLOW, &path);
	if (ret) {
		pr_warn(PH_LOG_PREFIX "%s not found (err=%d), skip\n",
			path_name, ret);
		return ret;
	}

	inode = d_inode(path.dentry);
	if (!inode || !inode->i_sb) {
		ph_invoke_path_put(&path);
		pr_warn(PH_LOG_PREFIX "%s has no inode, skip\n", path_name);
		return -ENOENT;
	}

	targets[target_count].ino = inode->i_ino;
	targets[target_count].dev = inode->i_sb->s_dev;
	targets[target_count].inode_ok = inode->i_ino != 0;
	strscpy(targets[target_count].path, path_name,
		sizeof(targets[target_count].path));
	pr_info(PH_LOG_PREFIX "target[%u] %s ino=%llu dev=%u:%u\n",
		target_count, path_name, targets[target_count].ino,
		MAJOR(targets[target_count].dev),
		MINOR(targets[target_count].dev));
	target_count++;
	ph_invoke_path_put(&path);

	return 0;
}

/*
 * Read the plain-text config file (one absolute path per line).
 * Runs in the insmod caller's process context, so filp_open() sees the
 * caller's mount namespace and credentials.
 */
static int load_config_file(const char *file_path)
{
	struct file *f;
	char *buf;
	loff_t pos = 0;
	ssize_t n;
	char *cursor, *line;

	if (!ph_filp_open || !ph_kernel_read) {
		pr_warn(PH_LOG_PREFIX
			"filp_open/kernel_read unavailable; cannot read %s\n",
			file_path);
		return -EOPNOTSUPP;
	}

	f = ph_invoke_filp_open(file_path, O_RDONLY, 0);
	if (IS_ERR(f)) {
		pr_warn(PH_LOG_PREFIX "cannot open %s (err=%ld)\n",
			file_path, PTR_ERR(f));
		return PTR_ERR(f);
	}

	buf = kmalloc(CONFIG_FILE_MAX, GFP_KERNEL);
	if (!buf) {
		filp_close(f, NULL);
		return -ENOMEM;
	}

	n = ph_invoke_kernel_read(f, buf, CONFIG_FILE_MAX - 1, &pos);
	filp_close(f, NULL);

	if (n <= 0) {
		pr_warn(PH_LOG_PREFIX "empty or unreadable config %s (n=%zd)\n",
			file_path, n);
		kfree(buf);
		return n < 0 ? (int)n : -ENOENT;
	}
	buf[n] = '\0';

	cursor = buf;
	while ((line = strsep(&cursor, "\n")) != NULL) {
		size_t len = strlen(line);

		/* strip trailing \r / spaces / tabs */
		while (len > 0 && (line[len - 1] == '\r' ||
				   line[len - 1] == ' ' ||
				   line[len - 1] == '\t'))
			line[--len] = '\0';

		/* skip leading spaces / tabs */
		while (*line == ' ' || *line == '\t')
			line++;

		if (!*line || *line == '#')
			continue;

		if (*line != '/') {
			pr_warn(PH_LOG_PREFIX "skip non-absolute path: %s\n",
				line);
			continue;
		}

		if (strlen(line) >= TARGET_TEXT_LEN) {
			pr_warn(PH_LOG_PREFIX "path too long, skip: %s\n",
				line);
			continue;
		}

		add_target_path(line);
	}

	kfree(buf);

	if (!target_count) {
		pr_warn(PH_LOG_PREFIX "no usable target in %s\n", file_path);
		return -ENOENT;
	}

	return 0;
}

static int resolve_target_paths(const char *paths)
{
	char *buf, *cursor, *item;

	buf = kstrdup(paths, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	cursor = buf;
	while ((item = strsep(&cursor, ",")) != NULL) {
		item = strim(item);
		if (!*item)
			continue;

		if (*item != '/') {
			pr_warn(PH_LOG_PREFIX "skip non-absolute path: %s\n",
				item);
			continue;
		}

		add_target_path(item);
	}

	kfree(buf);

	return target_count ? 0 : -ENOENT;
}

/*
 * VFS-level hooks.
 *
 * inode_permission's signature differs across kernel versions:
 *   < 5.12: int inode_permission(struct inode *, int)
 *   5.12+:  int inode_permission(struct user_namespace *, struct inode *, int)
 *   6.3+:   int inode_permission(struct mnt_idmap *, struct inode *, int)
 * which keeps the inode argument in x1 starting with 5.12.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 12, 0)
#define PH_PERM_INODE_REG 1
#else
#define PH_PERM_INODE_REG 0
#endif

static struct kretprobe kp_inode_perm;
static struct kretprobe kp_inode_getattr;

struct inode_perm_data {
	bool matched;
};

static atomic_t ph_perm_seen = ATOMIC_INIT(0);
static atomic_t ph_getattr_seen = ATOMIC_INIT(0);

static int perm_inode_entry(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct inode_perm_data *d = (struct inode_perm_data *)ri->data;
	struct inode *inode = (struct inode *)regs->regs[PH_PERM_INODE_REG];

	if (atomic_cmpxchg(&ph_perm_seen, 0, 1) == 0)
		pr_info(PH_LOG_PREFIX "inode_permission hook fired (first time)\n");

	d->matched = is_target_inode(inode);
	return 0;
}

static int perm_exit(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct inode_perm_data *d = (struct inode_perm_data *)ri->data;

	if (d->matched)
		regs_set_return_value(regs, -ENOENT);
	return 0;
}

static int getattr_entry(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct inode_perm_data *d = (struct inode_perm_data *)ri->data;
	struct path *path = (struct path *)regs->regs[0];
	struct inode *inode = NULL;

	if (atomic_cmpxchg(&ph_getattr_seen, 0, 1) == 0)
		pr_info(PH_LOG_PREFIX "vfs_getattr hook fired (first time)\n");

	if (path && path->dentry)
		inode = d_inode(path->dentry);

	d->matched = is_target_inode(inode);
	return 0;
}

static int getattr_exit(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct inode_perm_data *d = (struct inode_perm_data *)ri->data;

	if (d->matched)
		regs_set_return_value(regs, -ENOENT);
	return 0;
}

/*
 * Syscall entry-stub hooks. All of the hooked syscalls share the prefix
 *   sys_xxxat(int dfd, const char __user *filename, ...)
 * so the user filename pointer always lands in user_regs->regs[1].
 */
struct syscall_match_data {
	bool matched;
	bool needs_close;
};

static atomic_t ph_syscall_seen = ATOMIC_INIT(0);

static bool sys_path_match_user_filename(struct pt_regs *regs)
{
	struct pt_regs *user_regs = (struct pt_regs *)regs->regs[0];
	char buf[TARGET_TEXT_LEN];
	long len;

	if (!user_regs)
		return false;

	len = strncpy_from_user(buf, (const char __user *)user_regs->regs[1],
				sizeof(buf));
	if (len <= 0 || len >= sizeof(buf))
		return false;

	return sys_path_matches_target(buf);
}

static int sys_path_entry(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct syscall_match_data *d = (struct syscall_match_data *)ri->data;

	d->matched = sys_path_match_user_filename(regs);
	d->needs_close = false;

	if (d->matched &&
	    atomic_cmpxchg(&ph_syscall_seen, 0, 1) == 0)
		pr_info(PH_LOG_PREFIX "syscall path hook fired (first time)\n");

	return 0;
}

/*
 * openat / openat2: we may only fake -ENOENT when we can also release
 * the fd the syscall body just allocated; without close_fd() the fd
 * would leak, so leave the open untouched in that case.
 */
static int sys_openat_entry(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct syscall_match_data *d = (struct syscall_match_data *)ri->data;

	d->matched = false;
	d->needs_close = false;

	if (!ph_close_fd)
		return 0;

	if (!sys_path_match_user_filename(regs))
		return 0;

	d->matched = true;
	d->needs_close = true;

	if (atomic_cmpxchg(&ph_syscall_seen, 0, 1) == 0)
		pr_info(PH_LOG_PREFIX "syscall path hook fired (first time)\n");

	return 0;
}

static int sys_path_exit(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct syscall_match_data *d = (struct syscall_match_data *)ri->data;
	long ret = (long)regs->regs[0];

	if (!d->matched)
		return 0;

	if (d->needs_close && ret >= 0)
		ph_invoke_close_fd((unsigned int)ret);

	regs_set_return_value(regs, -ENOENT);
	return 0;
}

/*
 * The probe table keeps one descriptor (and its own entry handler) per
 * syscall kind, so we never have to fish `struct kretprobe *` back out of
 * the live `struct kretprobe_instance` (whose layout differs across KMIs).
 */
typedef int (*ph_syscall_entry_t)(struct kretprobe_instance *,
				  struct pt_regs *);

static struct ph_syscall_probe {
	const char *symbol;
	ph_syscall_entry_t entry;
	struct kretprobe rp;
	bool registered;
} ph_syscall_probes[] = {
	{ .symbol = "__arm64_sys_newfstatat", .entry = sys_path_entry   },
	{ .symbol = "__arm64_sys_statx",      .entry = sys_path_entry   },
	{ .symbol = "__arm64_sys_faccessat",  .entry = sys_path_entry   },
	{ .symbol = "__arm64_sys_faccessat2", .entry = sys_path_entry   },
	{ .symbol = "__arm64_sys_readlinkat", .entry = sys_path_entry   },
	{ .symbol = "__arm64_sys_openat",     .entry = sys_openat_entry },
	{ .symbol = "__arm64_sys_openat2",    .entry = sys_openat_entry },
};

static void register_syscall_hooks(void)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(ph_syscall_probes); i++) {
		struct ph_syscall_probe *p = &ph_syscall_probes[i];
		int ret;

		p->rp.kp.symbol_name = p->symbol;
		p->rp.entry_handler = p->entry;
		p->rp.handler = sys_path_exit;
		p->rp.data_size = sizeof(struct syscall_match_data);
		p->rp.maxactive = 40;

		ret = register_kretprobe(&p->rp);
		if (ret) {
			/* Tolerate per-symbol failure, keep the rest. */
			pr_warn(PH_LOG_PREFIX
				"register_kretprobe(%s) failed: %d (skip)\n",
				p->symbol, ret);
			continue;
		}
		p->registered = true;
		pr_info(PH_LOG_PREFIX "hooked %s\n", p->symbol);
	}
}

static void unregister_syscall_hooks(void)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(ph_syscall_probes); i++) {
		struct ph_syscall_probe *p = &ph_syscall_probes[i];

		if (p->registered) {
			unregister_kretprobe(&p->rp);
			p->registered = false;
		}
	}
}

/*
 * getdents64 directory-listing filter. The entry handler snapshots the
 * user buffer geometry and pre-allocates a kernel bounce buffer; the
 * exit handler copies the result back in, splices out every entry whose
 * d_ino matches a target, then writes the shortened buffer back to
 * userspace and fixes up the syscall return value.
 */
static struct kretprobe kp_getdents;
static bool getdents_registered;
static atomic_t ph_getdents_seen = ATOMIC_INIT(0);

struct getdents_cb_data {
	struct linux_dirent64 __user *dirent;
	void *kbuf;
	size_t kbuf_len;
};

static int getdents_entry(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct getdents_cb_data *d = (struct getdents_cb_data *)ri->data;
	struct pt_regs *user_regs = (struct pt_regs *)regs->regs[0];
	unsigned int count;

	d->dirent = NULL;
	d->kbuf = NULL;
	d->kbuf_len = 0;

	if (!user_regs)
		return 0;

	count = (unsigned int)user_regs->regs[2];
	d->dirent = (struct linux_dirent64 __user *)user_regs->regs[1];

	count = min(count, GETDENTS_BUF_LIMIT);
	if (!count)
		return 0;

	d->kbuf = kmalloc(count, GFP_KERNEL);
	if (d->kbuf)
		d->kbuf_len = count;
	return 0;
}

static int getdents_exit(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct getdents_cb_data *d = (struct getdents_cb_data *)ri->data;
	long ret = regs->regs[0];
	struct linux_dirent64 *kbuf, *prev, *cur;
	long bpos, new_len;
	const size_t hdr_off = offsetof(struct linux_dirent64, d_name);
	const size_t min_reclen = offsetof(struct linux_dirent64, d_name) + 1;
	bool modified = false;

	if (ret <= 0 || !d->dirent || !d->kbuf)
		goto out;

	if ((size_t)ret > d->kbuf_len) {
		pr_debug_ratelimited(PH_LOG_PREFIX
				     "getdents return too large (%ld > %zu), skip filtering\n",
				     ret, d->kbuf_len);
		goto out;
	}

	if (copy_from_user(d->kbuf, d->dirent, ret))
		goto out;

	kbuf = d->kbuf;
	prev = NULL;
	bpos = 0;
	new_len = ret;

	while (bpos + (long)hdr_off < new_len) {
		unsigned short reclen;

		cur = (struct linux_dirent64 *)((char *)kbuf + bpos);
		reclen = cur->d_reclen;

		if (reclen < min_reclen || reclen > new_len - bpos)
			break;

		if (is_target_ino(cur->d_ino)) {
			modified = true;
			if (atomic_cmpxchg(&ph_getdents_seen, 0, 1) == 0)
				pr_info(PH_LOG_PREFIX
					"getdents64 hook hid an entry (first time)\n");

			if (prev) {
				/* merge into the previous entry */
				if ((unsigned int)prev->d_reclen + reclen <=
				    65535u) {
					prev->d_reclen += reclen;
					bpos += reclen;
					continue;
				}
			}

			/* first entry in the buffer: shift the rest left */
			new_len -= reclen;
			if (new_len > bpos)
				memmove(cur, (char *)cur + reclen,
					new_len - bpos);
			continue;
		}

		prev = cur;
		bpos += reclen;
	}

	if (modified) {
		if (copy_to_user(d->dirent, kbuf, new_len))
			pr_warn_ratelimited(PH_LOG_PREFIX
					    "copy_to_user failed, directory may leak\n");
		else
			regs->regs[0] = new_len;
	}

out:
	kfree(d->kbuf);
	d->kbuf = NULL;
	d->kbuf_len = 0;
	return 0;
}

static int __init pathhide_init(void)
{
	int ret;

	ret = resolve_path_helpers();
	if (ret) {
		pr_err(PH_LOG_PREFIX "could not resolve VFS path helpers (err=%d)\n",
		       ret);
		return ret;
	}

	if (config_path && config_path[0]) {
		ret = load_config_file(config_path);
		if (ret)
			pr_warn(PH_LOG_PREFIX
				"config file %s unusable (err=%d), trying target_paths\n",
				config_path, ret);
	}

	if (!target_count && target_paths[0])
		resolve_target_paths(target_paths);

	if (!target_count) {
		pr_err(PH_LOG_PREFIX "no valid targets resolved\n");
		return -ENOENT;
	}

	kp_inode_perm.kp.symbol_name = "inode_permission";
	kp_inode_perm.entry_handler = perm_inode_entry;
	kp_inode_perm.handler = perm_exit;
	kp_inode_perm.data_size = sizeof(struct inode_perm_data);
	kp_inode_perm.maxactive = 40;
	ret = register_kretprobe(&kp_inode_perm);
	if (ret) {
		pr_err(PH_LOG_PREFIX
		       "register_kretprobe(inode_permission) failed: %d\n", ret);
		return ret;
	}
	pr_info(PH_LOG_PREFIX "hooked inode_permission\n");

	kp_inode_getattr.kp.symbol_name = "vfs_getattr";
	kp_inode_getattr.entry_handler = getattr_entry;
	kp_inode_getattr.handler = getattr_exit;
	kp_inode_getattr.data_size = sizeof(struct inode_perm_data);
	kp_inode_getattr.maxactive = 40;
	ret = register_kretprobe(&kp_inode_getattr);
	if (ret) {
		pr_err(PH_LOG_PREFIX
		       "register_kretprobe(vfs_getattr) failed: %d\n", ret);
		unregister_kretprobe(&kp_inode_perm);
		return ret;
	}
	pr_info(PH_LOG_PREFIX "hooked vfs_getattr\n");

	register_syscall_hooks();

	if (hide_dirents) {
		kp_getdents.kp.symbol_name = "__arm64_sys_getdents64";
		kp_getdents.entry_handler = getdents_entry;
		kp_getdents.handler = getdents_exit;
		kp_getdents.data_size = sizeof(struct getdents_cb_data);
		kp_getdents.maxactive = 20;
		ret = register_kretprobe(&kp_getdents);
		if (ret) {
			pr_warn(PH_LOG_PREFIX
				"register_kretprobe(__arm64_sys_getdents64) failed: %d; listings may leak\n",
				ret);
		} else {
			getdents_registered = true;
			pr_info(PH_LOG_PREFIX "hooked __arm64_sys_getdents64\n");
		}
	} else {
		pr_info(PH_LOG_PREFIX
			"hide_dirents=0, directory listings are not filtered\n");
	}

	pr_info(PH_LOG_PREFIX "loaded -- %u target(s) hidden globally\n",
		target_count);
	return 0;
}

static void __exit pathhide_exit(void)
{
	unregister_kretprobe(&kp_inode_perm);
	unregister_kretprobe(&kp_inode_getattr);
	unregister_syscall_hooks();
	if (getdents_registered) {
		unregister_kretprobe(&kp_getdents);
		getdents_registered = false;
	}

	pr_info(PH_LOG_PREFIX "unloaded -- %u target(s) visible again\n",
		target_count);
}

module_init(pathhide_init);
module_exit(pathhide_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("PathHide");
MODULE_DESCRIPTION("Simple selective path hiding via kretprobes (modeled on LKM-PathMask)");
