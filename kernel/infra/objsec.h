/* Minimal objsec.h for KernelSU build */
#ifndef _KERNELSU_OBJSEC_H
#define _KERNELSU_OBJSEC_H
#include <linux/types.h>
struct inode_security_struct {
	u32 sid;
};
static inline struct inode_security_struct *selinux_inode(struct inode *inode)
{
	return (struct inode_security_struct *)inode->i_security;
}
#endif
