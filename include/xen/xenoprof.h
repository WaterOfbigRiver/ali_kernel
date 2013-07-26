/******************************************************************************
 * xen/xenoprof.h
 *
 * Copyright (c) 2006 Isaku Yamahata <yamahata at valinux co jp>
 *                    VA Linux Systems Japan K.K.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#ifndef __XEN_XENOPROF_H__
#define __XEN_XENOPROF_H__
#ifdef CONFIG_XEN

#if 0
#include <asm/xenoprof.h>
#endif

#if defined(CONFIG_X86) || defined(CONFIG_X86_64)
/* xenoprof x86 specific */
struct super_block;
struct dentry;
int xenoprof_create_files(struct super_block * sb, struct dentry * root);
#define HAVE_XENOPROF_CREATE_FILES

struct xenoprof_init;
void xenoprof_arch_init_counter(struct xenoprof_init *init);
void xenoprof_arch_counter(void);
void xenoprof_arch_start(void);
void xenoprof_arch_stop(void);

struct xenoprof_arch_shared_buffer {
	/* nothing */
};
struct xenoprof_shared_buffer;
void xenoprof_arch_unmap_shared_buffer(struct xenoprof_shared_buffer* sbuf);
struct xenoprof_get_buffer;
int xenoprof_arch_map_shared_buffer(struct xenoprof_get_buffer* get_buffer, struct xenoprof_shared_buffer* sbuf);
struct xenoprof_passive;
int xenoprof_arch_set_passive(struct xenoprof_passive* pdomain, struct xenoprof_shared_buffer* sbuf);
#endif

/* xenoprof common */
struct oprofile_operations;
int xenoprofile_init(struct oprofile_operations * ops);
void xenoprofile_exit(void);

struct xenoprof_shared_buffer {
	char					*buffer;
	struct xenoprof_arch_shared_buffer	arch;
};
#else
#define xenoprofile_init(ops)	(-ENOSYS)
#define xenoprofile_exit()	do { } while (0)

#endif /* CONFIG_XEN */
#endif /* __XEN_XENOPROF_H__ */
