/*
 * Xen mmu operations
 *
 * This file contains the various mmu fetch and update operations.
 * The most important job they must perform is the mapping between the
 * domain's pfn and the overall machine mfns.
 *
 * Xen allows guests to directly update the pagetable, in a controlled
 * fashion.  In other words, the guest modifies the same pagetable
 * that the CPU actually uses, which eliminates the overhead of having
 * a separate shadow pagetable.
 *
 * In order to allow this, it falls on the guest domain to map its
 * notion of a "physical" pfn - which is just a domain-local linear
 * address - into a real "machine address" which the CPU's MMU can
 * use.
 *
 * A pgd_t/pmd_t/pte_t will typically contain an mfn, and so can be
 * inserted directly into the pagetable.  When creating a new
 * pte/pmd/pgd, it converts the passed pfn into an mfn.  Conversely,
 * when reading the content back with __(pgd|pmd|pte)_val, it converts
 * the mfn back into a pfn.
 *
 * The other constraint is that all pages which make up a pagetable
 * must be mapped read-only in the guest.  This prevents uncontrolled
 * guest updates to the pagetable.  Xen strictly enforces this, and
 * will disallow any pagetable update which will end up mapping a
 * pagetable page RW, and will disallow using any writable page as a
 * pagetable.
 *
 * Naively, when loading %cr3 with the base of a new pagetable, Xen
 * would need to validate the whole pagetable before going on.
 * Naturally, this is quite slow.  The solution is to "pin" a
 * pagetable, which enforces all the constraints on the pagetable even
 * when it is not actively in use.  This menas that Xen can be assured
 * that it is still valid when you do load it into %cr3, and doesn't
 * need to revalidate it.
 *
 * Jeremy Fitzhardinge <jeremy@xensource.com>, XenSource Inc, 2007
 */
#include <linux/sched.h>
#include <linux/highmem.h>
#include <linux/debugfs.h>
#include <linux/bug.h>
#include <linux/vmalloc.h>
#include <linux/module.h>

#include <asm/pgtable.h>
#include <asm/tlbflush.h>
#include <asm/fixmap.h>
#include <asm/mmu_context.h>
#include <asm/setup.h>
#include <asm/paravirt.h>
#include <asm/e820.h>
#include <asm/linkage.h>
#include <asm/pat.h>
#include <asm/init.h>
#include <asm/page.h>

#include <asm/xen/hypercall.h>
#include <asm/xen/hypervisor.h>

#include <xen/xen.h>
#include <xen/page.h>
#include <xen/interface/xen.h>
#include <xen/interface/hvm/hvm_op.h>
#include <xen/interface/version.h>
#include <xen/interface/memory.h>
#include <xen/hvc-console.h>

#include "multicalls.h"
#include "mmu.h"
#include "debugfs.h"

#define MMU_UPDATE_HISTO	30

/*
 * Protects atomic reservation decrease/increase against concurrent increases.
 * Also protects non-atomic updates of current_pages and driver_pages, and
 * balloon lists.
 */
DEFINE_SPINLOCK(xen_reservation_lock);

#ifdef CONFIG_XEN_DEBUG_FS

static struct {
	u32 pgd_update;
	u32 pgd_update_pinned;
	u32 pgd_update_batched;

	u32 pud_update;
	u32 pud_update_pinned;
	u32 pud_update_batched;

	u32 pmd_update;
	u32 pmd_update_pinned;
	u32 pmd_update_batched;

	u32 pte_update;
	u32 pte_update_pinned;
	u32 pte_update_batched;

	u32 mmu_update;
	u32 mmu_update_extended;
	u32 mmu_update_histo[MMU_UPDATE_HISTO];

	u32 prot_commit;
	u32 prot_commit_batched;

	u32 set_pte_at;
	u32 set_pte_at_batched;
	u32 set_pte_at_pinned;
	u32 set_pte_at_current;
	u32 set_pte_at_kernel;
} mmu_stats;

static u8 zero_stats;

static inline void check_zero(void)
{
	if (unlikely(zero_stats)) {
		memset(&mmu_stats, 0, sizeof(mmu_stats));
		zero_stats = 0;
	}
}

#define ADD_STATS(elem, val)			\
	do { check_zero(); mmu_stats.elem += (val); } while(0)

#else  /* !CONFIG_XEN_DEBUG_FS */

#define ADD_STATS(elem, val)	do { (void)(val); } while(0)

#endif /* CONFIG_XEN_DEBUG_FS */


/*
 * Identity map, in addition to plain kernel map.  This needs to be
 * large enough to allocate page table pages to allocate the rest.
 * Each page can map 2MB.
 */
#define LEVEL1_IDENT_ENTRIES	(PTRS_PER_PTE * 4)
static RESERVE_BRK_ARRAY(pte_t, level1_ident_pgt, LEVEL1_IDENT_ENTRIES);

#ifdef CONFIG_X86_64
/* l3 pud for userspace vsyscall mapping */
static pud_t level3_user_vsyscall[PTRS_PER_PUD] __page_aligned_bss;
#endif /* CONFIG_X86_64 */

/*
 * Note about cr3 (pagetable base) values:
 *
 * xen_cr3 contains the current logical cr3 value; it contains the
 * last set cr3.  This may not be the current effective cr3, because
 * its update may be being lazily deferred.  However, a vcpu looking
 * at its own cr3 can use this value knowing that it everything will
 * be self-consistent.
 *
 * xen_current_cr3 contains the actual vcpu cr3; it is set once the
 * hypercall to set the vcpu cr3 is complete (so it may be a little
 * out of date, but it will never be set early).  If one vcpu is
 * looking at another vcpu's cr3 value, it should use this variable.
 */
DEFINE_PER_CPU(unsigned long, xen_cr3);	 /* cr3 stored as physaddr */
DEFINE_PER_CPU(unsigned long, xen_current_cr3);	 /* actual vcpu cr3 */


/*
 * Just beyond the highest usermode address.  STACK_TOP_MAX has a
 * redzone above it, so round it up to a PGD boundary.
 */
#define USER_LIMIT	((STACK_TOP_MAX + PGDIR_SIZE - 1) & PGDIR_MASK)

/*
 * Xen leaves the responsibility for maintaining p2m mappings to the
 * guests themselves, but it must also access and update the p2m array
 * during suspend/resume when all the pages are reallocated.
 *
 * The p2m table is logically a flat array, but we implement it as a
 * three-level tree to allow the address space to be sparse.
 *
 *                               Xen
 *                                |
 *     p2m_top              p2m_top_mfn
 *       /  \                   /   \
 * p2m_mid p2m_mid	p2m_mid_mfn p2m_mid_mfn
 *    / \      / \         /           /
 *  p2m p2m p2m p2m p2m p2m p2m ...
 *
 * The p2m_mid_mfn pages are mapped by p2m_top_mfn_p.
 *
 * The p2m_top and p2m_top_mfn levels are limited to 1 page, so the
 * maximum representable pseudo-physical address space is:
 *  P2M_TOP_PER_PAGE * P2M_MID_PER_PAGE * P2M_PER_PAGE pages
 *
 * P2M_PER_PAGE depends on the architecture, as a mfn is always
 * unsigned long (8 bytes on 64-bit, 4 bytes on 32), leading to
 * 512 and 1024 entries respectively. 
 */

unsigned long xen_max_p2m_pfn __read_mostly;

#define P2M_PER_PAGE		(PAGE_SIZE / sizeof(unsigned long))
#define P2M_MID_PER_PAGE	(PAGE_SIZE / sizeof(unsigned long *))
#define P2M_TOP_PER_PAGE	(PAGE_SIZE / sizeof(unsigned long **))

#define MAX_P2M_PFN		(P2M_TOP_PER_PAGE * P2M_MID_PER_PAGE * P2M_PER_PAGE)

/* Placeholders for holes in the address space */
static RESERVE_BRK_ARRAY(unsigned long, p2m_missing, P2M_PER_PAGE);
static RESERVE_BRK_ARRAY(unsigned long *, p2m_mid_missing, P2M_MID_PER_PAGE);
static RESERVE_BRK_ARRAY(unsigned long, p2m_mid_missing_mfn, P2M_MID_PER_PAGE);

static RESERVE_BRK_ARRAY(unsigned long **, p2m_top, P2M_TOP_PER_PAGE);
static RESERVE_BRK_ARRAY(unsigned long, p2m_top_mfn, P2M_TOP_PER_PAGE);
static RESERVE_BRK_ARRAY(unsigned long *, p2m_top_mfn_p, P2M_TOP_PER_PAGE);

RESERVE_BRK(p2m_mid, PAGE_SIZE * (MAX_DOMAIN_PAGES / (P2M_PER_PAGE * P2M_MID_PER_PAGE)));
RESERVE_BRK(p2m_mid_mfn, PAGE_SIZE * (MAX_DOMAIN_PAGES / (P2M_PER_PAGE * P2M_MID_PER_PAGE)));

static inline unsigned p2m_top_index(unsigned long pfn)
{
	BUG_ON(pfn >= MAX_P2M_PFN);
	return pfn / (P2M_MID_PER_PAGE * P2M_PER_PAGE);
}

static inline unsigned p2m_mid_index(unsigned long pfn)
{
	return (pfn / P2M_PER_PAGE) % P2M_MID_PER_PAGE;
}

static inline unsigned p2m_index(unsigned long pfn)
{
	return pfn % P2M_PER_PAGE;
}

static void p2m_top_init(unsigned long ***top)
{
	unsigned i;

	for (i = 0; i < P2M_TOP_PER_PAGE; i++)
		top[i] = p2m_mid_missing;
}

static void p2m_top_mfn_init(unsigned long *top)
{
	unsigned i;

	for (i = 0; i < P2M_TOP_PER_PAGE; i++)
		top[i] = virt_to_mfn(p2m_mid_missing_mfn);
}

static void p2m_top_mfn_p_init(unsigned long **top)
{
	unsigned i;

	for (i = 0; i < P2M_TOP_PER_PAGE; i++)
		top[i] = p2m_mid_missing_mfn;
}

static void p2m_mid_init(unsigned long **mid)
{
	unsigned i;

	for (i = 0; i < P2M_MID_PER_PAGE; i++)
		mid[i] = p2m_missing;
}

static void p2m_mid_mfn_init(unsigned long *mid)
{
	unsigned i;

	for (i = 0; i < P2M_MID_PER_PAGE; i++)
		mid[i] = virt_to_mfn(p2m_missing);
}

static void p2m_init(unsigned long *p2m)
{
	unsigned i;

	for (i = 0; i < P2M_MID_PER_PAGE; i++)
		p2m[i] = INVALID_P2M_ENTRY;
}

static int lookup_pte_fn(
	pte_t *pte, struct page *pmd_page, unsigned long addr, void *data)
{
	uint64_t *ptep = (uint64_t *)data;
	if (ptep)
		*ptep = ((uint64_t)pfn_to_mfn(page_to_pfn(pmd_page)) <<
			 PAGE_SHIFT) | ((unsigned long)pte & ~PAGE_MASK);
	return 0;
}

int create_lookup_pte_addr(struct mm_struct *mm,
			   unsigned long address,
			   uint64_t *ptep)
{
	return apply_to_page_range(mm, address, PAGE_SIZE,
				   lookup_pte_fn, ptep);
}

EXPORT_SYMBOL(create_lookup_pte_addr);

/*
 * Build the parallel p2m_top_mfn and p2m_mid_mfn structures
 *
 * This is called both at boot time, and after resuming from suspend:
 * - At boot time we're called very early, and must use extend_brk()
 *   to allocate memory.
 *
 * - After resume we're called from within stop_machine, but the mfn
 *   tree should alreay be completely allocated.
 */
void xen_build_mfn_list_list(void)
{
	unsigned long pfn;

	/* Pre-initialize p2m_top_mfn to be completely missing */
	if (p2m_top_mfn == NULL) {
		p2m_mid_missing_mfn = extend_brk(PAGE_SIZE, PAGE_SIZE);
		p2m_mid_mfn_init(p2m_mid_missing_mfn);

		p2m_top_mfn_p = extend_brk(PAGE_SIZE, PAGE_SIZE);
		p2m_top_mfn_p_init(p2m_top_mfn_p);

		p2m_top_mfn = extend_brk(PAGE_SIZE, PAGE_SIZE);
		p2m_top_mfn_init(p2m_top_mfn);
	} else {
		/* Reinitialise, mfn's all change after migration */
		p2m_mid_mfn_init(p2m_mid_missing_mfn);
	}

	for (pfn = 0; pfn < xen_max_p2m_pfn; pfn += P2M_PER_PAGE) {
		unsigned topidx = p2m_top_index(pfn);
		unsigned mididx = p2m_mid_index(pfn);
		unsigned long **mid;
		unsigned long *mid_mfn_p;

		mid = p2m_top[topidx];
		mid_mfn_p = p2m_top_mfn_p[topidx];

		/* Don't bother allocating any mfn mid levels if
		 * they're just missing, just update the stored mfn,
		 * since all could have changed over a migrate.
		 */
		if (mid == p2m_mid_missing) {
			BUG_ON(mididx);
			BUG_ON(mid_mfn_p != p2m_mid_missing_mfn);
			p2m_top_mfn[topidx] = virt_to_mfn(p2m_mid_missing_mfn);
			pfn += (P2M_MID_PER_PAGE - 1) * P2M_PER_PAGE;
			continue;
		}

		if (mid_mfn_p == p2m_mid_missing_mfn) {
			/*
			 * XXX boot-time only!  We should never find
			 * missing parts of the mfn tree after
			 * runtime.  extend_brk() will BUG if we call
			 * it too late.
			 */
			mid_mfn_p = extend_brk(PAGE_SIZE, PAGE_SIZE);
			p2m_mid_mfn_init(mid_mfn_p);

			p2m_top_mfn_p[topidx] = mid_mfn_p;
		}

		p2m_top_mfn[topidx] = virt_to_mfn(mid_mfn_p);
		mid_mfn_p[mididx] = virt_to_mfn(mid[mididx]);
	}
}

void xen_setup_mfn_list_list(void)
{
	BUG_ON(HYPERVISOR_shared_info == &xen_dummy_shared_info);

	HYPERVISOR_shared_info->arch.pfn_to_mfn_frame_list_list =
		virt_to_mfn(p2m_top_mfn);
	HYPERVISOR_shared_info->arch.max_pfn = xen_max_p2m_pfn;
}

/* Set up p2m_top to point to the domain-builder provided p2m pages */
void __init xen_build_dynamic_phys_to_machine(void)
{
	unsigned long *mfn_list = (unsigned long *)xen_start_info->mfn_list;
	unsigned long max_pfn = min(MAX_DOMAIN_PAGES, xen_start_info->nr_pages);
	unsigned long pfn;

	xen_max_p2m_pfn = max_pfn;

	p2m_missing = extend_brk(PAGE_SIZE, PAGE_SIZE);
	p2m_init(p2m_missing);

	p2m_mid_missing = extend_brk(PAGE_SIZE, PAGE_SIZE);
	p2m_mid_init(p2m_mid_missing);

	p2m_top = extend_brk(PAGE_SIZE, PAGE_SIZE);
	p2m_top_init(p2m_top);

	/*
	 * The domain builder gives us a pre-constructed p2m array in
	 * mfn_list for all the pages initially given to us, so we just
	 * need to graft that into our tree structure.
	 */
	for (pfn = 0; pfn < max_pfn; pfn += P2M_PER_PAGE) {
		unsigned topidx = p2m_top_index(pfn);
		unsigned mididx = p2m_mid_index(pfn);

		if (p2m_top[topidx] == p2m_mid_missing) {
			unsigned long **mid = extend_brk(PAGE_SIZE, PAGE_SIZE);
			p2m_mid_init(mid);

			p2m_top[topidx] = mid;
		}

		/*
		 * As long as the mfn_list has enough entries to completely
		 * fill a p2m page, pointing into the array is ok. But if
		 * not the entries beyond the last pfn will be undefined.
		 */
		if (unlikely(pfn + P2M_PER_PAGE > max_pfn)) {
			unsigned long p2midx;

			p2midx = max_pfn % P2M_PER_PAGE;
			for ( ; p2midx < P2M_PER_PAGE; p2midx++)
				mfn_list[pfn + p2midx] = INVALID_P2M_ENTRY;
		}
		p2m_top[topidx][mididx] = &mfn_list[pfn];
	}
}

unsigned long get_phys_to_machine(unsigned long pfn)
{
	unsigned topidx, mididx, idx;

	if (unlikely(pfn >= MAX_P2M_PFN))
		return INVALID_P2M_ENTRY;

	topidx = p2m_top_index(pfn);
	mididx = p2m_mid_index(pfn);
	idx = p2m_index(pfn);

	return p2m_top[topidx][mididx][idx];
}
EXPORT_SYMBOL_GPL(get_phys_to_machine);

static void *alloc_p2m_page(void)
{
	return (void *)__get_free_page(GFP_KERNEL | __GFP_REPEAT);
}

static void free_p2m_page(void *p)
{
	free_page((unsigned long)p);
}

/* 
 * Fully allocate the p2m structure for a given pfn.  We need to check
 * that both the top and mid levels are allocated, and make sure the
 * parallel mfn tree is kept in sync.  We may race with other cpus, so
 * the new pages are installed with cmpxchg; if we lose the race then
 * simply free the page we allocated and use the one that's there.
 */
static bool alloc_p2m(unsigned long pfn)
{
	unsigned topidx, mididx;
	unsigned long ***top_p, **mid;
	unsigned long *top_mfn_p, *mid_mfn;

	topidx = p2m_top_index(pfn);
	mididx = p2m_mid_index(pfn);

	top_p = &p2m_top[topidx];
	mid = *top_p;

	if (mid == p2m_mid_missing) {
		/* Mid level is missing, allocate a new one */
		mid = alloc_p2m_page();
		if (!mid)
			return false;

		p2m_mid_init(mid);

		if (cmpxchg(top_p, p2m_mid_missing, mid) != p2m_mid_missing)
			free_p2m_page(mid);
	}

	top_mfn_p = &p2m_top_mfn[topidx];
	mid_mfn = p2m_top_mfn_p[topidx];

	BUG_ON(virt_to_mfn(mid_mfn) != *top_mfn_p);

	if (mid_mfn == p2m_mid_missing_mfn) {
		/* Separately check the mid mfn level */
		unsigned long missing_mfn;
		unsigned long mid_mfn_mfn;

		mid_mfn = alloc_p2m_page();
		if (!mid_mfn)
			return false;

		p2m_mid_mfn_init(mid_mfn);

		missing_mfn = virt_to_mfn(p2m_mid_missing_mfn);
		mid_mfn_mfn = virt_to_mfn(mid_mfn);
		if (cmpxchg(top_mfn_p, missing_mfn, mid_mfn_mfn) != missing_mfn)
			free_p2m_page(mid_mfn);
		else
			p2m_top_mfn_p[topidx] = mid_mfn;
	}

	if (p2m_top[topidx][mididx] == p2m_missing) {
		/* p2m leaf page is missing */
		unsigned long *p2m;

		p2m = alloc_p2m_page();
		if (!p2m)
			return false;

		p2m_init(p2m);

		if (cmpxchg(&mid[mididx], p2m_missing, p2m) != p2m_missing)
			free_p2m_page(p2m);
		else
			mid_mfn[mididx] = virt_to_mfn(p2m);
	}

	return true;
}

/* Try to install p2m mapping; fail if intermediate bits missing */
bool __set_phys_to_machine(unsigned long pfn, unsigned long mfn)
{
	unsigned topidx, mididx, idx;

	if (unlikely(pfn >= MAX_P2M_PFN)) {
		BUG_ON(mfn != INVALID_P2M_ENTRY);
		return true;
	}

	topidx = p2m_top_index(pfn);
	mididx = p2m_mid_index(pfn);
	idx = p2m_index(pfn);

	if (p2m_top[topidx][mididx] == p2m_missing)
		return mfn == INVALID_P2M_ENTRY;

	p2m_top[topidx][mididx][idx] = mfn;

	return true;
}

bool set_phys_to_machine(unsigned long pfn, unsigned long mfn)
{
	if (unlikely(xen_feature(XENFEAT_auto_translated_physmap))) {
		BUG_ON(pfn != mfn && mfn != INVALID_P2M_ENTRY);
		return true;
	}

	if (unlikely(!__set_phys_to_machine(pfn, mfn)))  {
		if (!alloc_p2m(pfn))
			return false;

		if (!__set_phys_to_machine(pfn, mfn))
			return false;
	}

	return true;
}

unsigned long arbitrary_virt_to_mfn(void *vaddr)
{
	xmaddr_t maddr = arbitrary_virt_to_machine(vaddr);

	return PFN_DOWN(maddr.maddr);
}
EXPORT_SYMBOL_GPL(set_phys_to_machine);

xmaddr_t arbitrary_virt_to_machine(void *vaddr)
{
	unsigned long address = (unsigned long)vaddr;
	unsigned int level;
	pte_t *pte;
	unsigned offset;

	/*
	 * if the PFN is in the linear mapped vaddr range, we can just use
	 * the (quick) virt_to_machine() p2m lookup
	 */
	if (virt_addr_valid(vaddr))
		return virt_to_machine(vaddr);

	/* otherwise we have to do a (slower) full page-table walk */

	pte = lookup_address(address, &level);
	BUG_ON(pte == NULL);
	offset = address & ~PAGE_MASK;
	return XMADDR(((phys_addr_t)pte_mfn(*pte) << PAGE_SHIFT) + offset);
}

void make_lowmem_page_readonly(void *vaddr)
{
	pte_t *pte, ptev;
	unsigned long address = (unsigned long)vaddr;
	unsigned int level;

	pte = lookup_address(address, &level);
	if (pte == NULL)
		return;		/* vaddr missing */

	ptev = pte_wrprotect(*pte);

	if (HYPERVISOR_update_va_mapping(address, ptev, 0))
		BUG();
}

void make_lowmem_page_readwrite(void *vaddr)
{
	pte_t *pte, ptev;
	unsigned long address = (unsigned long)vaddr;
	unsigned int level;

	pte = lookup_address(address, &level);
	if (pte == NULL)
		return;		/* vaddr missing */

	ptev = pte_mkwrite(*pte);

	if (HYPERVISOR_update_va_mapping(address, ptev, 0))
		BUG();
}


static bool xen_page_pinned(void *ptr)
{
	struct page *page = virt_to_page(ptr);

	return PagePinned(page);
}

void xen_set_domain_pte(pte_t *ptep, pte_t pteval, unsigned domid)
{
	struct multicall_space mcs;
	struct mmu_update *u;

	mcs = xen_mc_entry(sizeof(*u));
	u = mcs.args;

	/* ptep might be kmapped when using 32-bit HIGHPTE */
	u->ptr = arbitrary_virt_to_machine(ptep).maddr;
	u->val = pte_val_ma(pteval);

	MULTI_mmu_update(mcs.mc, mcs.args, 1, NULL, domid);

	xen_mc_issue(PARAVIRT_LAZY_MMU);
}
EXPORT_SYMBOL_GPL(xen_set_domain_pte);

static void xen_extend_mmu_update(const struct mmu_update *update)
{
	struct multicall_space mcs;
	struct mmu_update *u;

	mcs = xen_mc_extend_args(__HYPERVISOR_mmu_update, sizeof(*u));

	if (mcs.mc != NULL) {
		ADD_STATS(mmu_update_extended, 1);
		ADD_STATS(mmu_update_histo[mcs.mc->args[1]], -1);

		mcs.mc->args[1]++;

		if (mcs.mc->args[1] < MMU_UPDATE_HISTO)
			ADD_STATS(mmu_update_histo[mcs.mc->args[1]], 1);
		else
			ADD_STATS(mmu_update_histo[0], 1);
	} else {
		ADD_STATS(mmu_update, 1);
		mcs = __xen_mc_entry(sizeof(*u));
		MULTI_mmu_update(mcs.mc, mcs.args, 1, NULL, DOMID_SELF);
		ADD_STATS(mmu_update_histo[1], 1);
	}

	u = mcs.args;
	*u = *update;
}

void xen_set_pmd_hyper(pmd_t *ptr, pmd_t val)
{
	struct mmu_update u;

	preempt_disable();

	xen_mc_batch();

	/* ptr may be ioremapped for 64-bit pagetable setup */
	u.ptr = arbitrary_virt_to_machine(ptr).maddr;
	u.val = pmd_val_ma(val);
	xen_extend_mmu_update(&u);

	ADD_STATS(pmd_update_batched, paravirt_get_lazy_mode() == PARAVIRT_LAZY_MMU);

	xen_mc_issue(PARAVIRT_LAZY_MMU);

	preempt_enable();
}

void xen_set_pmd(pmd_t *ptr, pmd_t val)
{
	ADD_STATS(pmd_update, 1);

	/* If page is not pinned, we can just update the entry
	   directly */
	if (!xen_page_pinned(ptr)) {
		*ptr = val;
		return;
	}

	ADD_STATS(pmd_update_pinned, 1);

	xen_set_pmd_hyper(ptr, val);
}

/*
 * Associate a virtual page frame with a given physical page frame
 * and protection flags for that frame.
 */
void set_pte_mfn(unsigned long vaddr, unsigned long mfn, pgprot_t flags)
{
	set_pte_vaddr(vaddr, mfn_pte(mfn, flags));
}

void xen_set_pte_at(struct mm_struct *mm, unsigned long addr,
		    pte_t *ptep, pte_t pteval)
{
	ADD_STATS(set_pte_at, 1);
//	ADD_STATS(set_pte_at_pinned, xen_page_pinned(ptep));
	ADD_STATS(set_pte_at_current, mm == current->mm);
	ADD_STATS(set_pte_at_kernel, mm == &init_mm);

	if (mm == current->mm || mm == &init_mm) {
		if (paravirt_get_lazy_mode() == PARAVIRT_LAZY_MMU) {
			struct multicall_space mcs;
			mcs = xen_mc_entry(0);

			MULTI_update_va_mapping(mcs.mc, addr, pteval, 0);
			ADD_STATS(set_pte_at_batched, 1);
			xen_mc_issue(PARAVIRT_LAZY_MMU);
			goto out;
		} else
			if (HYPERVISOR_update_va_mapping(addr, pteval, 0) == 0)
				goto out;
	}
	xen_set_pte(ptep, pteval);

out:	return;
}

pte_t xen_ptep_modify_prot_start(struct mm_struct *mm,
				 unsigned long addr, pte_t *ptep)
{
	/* Just return the pte as-is.  We preserve the bits on commit */
	return *ptep;
}

void xen_ptep_modify_prot_commit(struct mm_struct *mm, unsigned long addr,
				 pte_t *ptep, pte_t pte)
{
	struct mmu_update u;

	xen_mc_batch();

	u.ptr = arbitrary_virt_to_machine(ptep).maddr | MMU_PT_UPDATE_PRESERVE_AD;
	u.val = pte_val_ma(pte);
	xen_extend_mmu_update(&u);

	ADD_STATS(prot_commit, 1);
	ADD_STATS(prot_commit_batched, paravirt_get_lazy_mode() == PARAVIRT_LAZY_MMU);

	xen_mc_issue(PARAVIRT_LAZY_MMU);
}

/* Assume pteval_t is equivalent to all the other *val_t types. */
static pteval_t pte_mfn_to_pfn(pteval_t val)
{
	if (val & _PAGE_PRESENT) {
		unsigned long mfn = (val & PTE_PFN_MASK) >> PAGE_SHIFT;
		pteval_t flags = val & PTE_FLAGS_MASK;
		val = ((pteval_t)mfn_to_pfn(mfn) << PAGE_SHIFT) | flags;
	}

	return val;
}

static pteval_t pte_pfn_to_mfn(pteval_t val)
{
	if (val & _PAGE_PRESENT) {
		unsigned long pfn = (val & PTE_PFN_MASK) >> PAGE_SHIFT;
		pteval_t flags = val & PTE_FLAGS_MASK;
		unsigned long mfn = pfn_to_mfn(pfn);

		/*
		 * If there's no mfn for the pfn, then just create an
		 * empty non-present pte.  Unfortunately this loses
		 * information about the original pfn, so
		 * pte_mfn_to_pfn is asymmetric.
		 */
		if (unlikely(mfn == INVALID_P2M_ENTRY)) {
			mfn = 0;
			flags = 0;
		}

		val = ((pteval_t)mfn << PAGE_SHIFT) | flags;
	}

	return val;
}

static pteval_t iomap_pte(pteval_t val)
{
	if (val & _PAGE_PRESENT) {
		unsigned long pfn = (val & PTE_PFN_MASK) >> PAGE_SHIFT;
		pteval_t flags = val & PTE_FLAGS_MASK;

		/* We assume the pte frame number is a MFN, so
		   just use it as-is. */
		val = ((pteval_t)pfn << PAGE_SHIFT) | flags;
	}

	return val;
}

pteval_t xen_pte_val(pte_t pte)
{
	pteval_t pteval = pte.pte;

	/* If this is a WC pte, convert back from Xen WC to Linux WC */
	if ((pteval & (_PAGE_PAT | _PAGE_PCD | _PAGE_PWT)) == _PAGE_PAT) {
		WARN_ON(!pat_enabled);
		pteval = (pteval & ~_PAGE_PAT) | _PAGE_PWT;
	}

	if (xen_initial_domain() && (pteval & _PAGE_IOMAP))
		return pteval;

	return pte_mfn_to_pfn(pteval);
}
PV_CALLEE_SAVE_REGS_THUNK(xen_pte_val);

pgdval_t xen_pgd_val(pgd_t pgd)
{
	return pte_mfn_to_pfn(pgd.pgd);
}
PV_CALLEE_SAVE_REGS_THUNK(xen_pgd_val);

/*
 * Xen's PAT setup is part of its ABI, though I assume entries 6 & 7
 * are reserved for now, to correspond to the Intel-reserved PAT
 * types.
 *
 * We expect Linux's PAT set as follows:
 *
 * Idx  PTE flags        Linux    Xen    Default
 * 0                     WB       WB     WB
 * 1            PWT      WC       WT     WT
 * 2        PCD          UC-      UC-    UC-
 * 3        PCD PWT      UC       UC     UC
 * 4    PAT              WB       WC     WB
 * 5    PAT     PWT      WC       WP     WT
 * 6    PAT PCD          UC-      UC     UC-
 * 7    PAT PCD PWT      UC       UC     UC
 */

void xen_set_pat(u64 pat)
{
	/* We expect Linux to use a PAT setting of
	 * UC UC- WC WB (ignoring the PAT flag) */
	WARN_ON(pat != 0x0007010600070106ull);
}

pte_t xen_make_pte(pteval_t pte)
{
	phys_addr_t addr = (pte & PTE_PFN_MASK);

	/* If Linux is trying to set a WC pte, then map to the Xen WC.
	 * If _PAGE_PAT is set, then it probably means it is really
	 * _PAGE_PSE, so avoid fiddling with the PAT mapping and hope
	 * things work out OK...
	 *
	 * (We should never see kernel mappings with _PAGE_PSE set,
	 * but we could see hugetlbfs mappings, I think.).
	 */
	if (pat_enabled && !WARN_ON(pte & _PAGE_PAT)) {
		if ((pte & (_PAGE_PCD | _PAGE_PWT)) == _PAGE_PWT)
			pte = (pte & ~(_PAGE_PCD | _PAGE_PWT)) | _PAGE_PAT;
	}

	/*
	 * Unprivileged domains are allowed to do IOMAPpings for
	 * PCI passthrough, but not map ISA space.  The ISA
	 * mappings are just dummy local mappings to keep other
	 * parts of the kernel happy.
	 */
	if (unlikely(pte & _PAGE_IOMAP) &&
	    (xen_initial_domain() || addr >= ISA_END_ADDRESS)) {
		pte = iomap_pte(pte);
	} else {
		pte &= ~_PAGE_IOMAP;
		pte = pte_pfn_to_mfn(pte);
	}

	return native_make_pte(pte);
}
PV_CALLEE_SAVE_REGS_THUNK(xen_make_pte);

pgd_t xen_make_pgd(pgdval_t pgd)
{
	pgd = pte_pfn_to_mfn(pgd);
	return native_make_pgd(pgd);
}
PV_CALLEE_SAVE_REGS_THUNK(xen_make_pgd);

pmdval_t xen_pmd_val(pmd_t pmd)
{
	return pte_mfn_to_pfn(pmd.pmd);
}
PV_CALLEE_SAVE_REGS_THUNK(xen_pmd_val);

void xen_set_pud_hyper(pud_t *ptr, pud_t val)
{
	struct mmu_update u;

	preempt_disable();

	xen_mc_batch();

	/* ptr may be ioremapped for 64-bit pagetable setup */
	u.ptr = arbitrary_virt_to_machine(ptr).maddr;
	u.val = pud_val_ma(val);
	xen_extend_mmu_update(&u);

	ADD_STATS(pud_update_batched, paravirt_get_lazy_mode() == PARAVIRT_LAZY_MMU);

	xen_mc_issue(PARAVIRT_LAZY_MMU);

	preempt_enable();
}

void xen_set_pud(pud_t *ptr, pud_t val)
{
	ADD_STATS(pud_update, 1);

	/* If page is not pinned, we can just update the entry
	   directly */
	if (!xen_page_pinned(ptr)) {
		*ptr = val;
		return;
	}

	ADD_STATS(pud_update_pinned, 1);

	xen_set_pud_hyper(ptr, val);
}

void xen_set_pte(pte_t *ptep, pte_t pte)
{
	ADD_STATS(pte_update, 1);
//	ADD_STATS(pte_update_pinned, xen_page_pinned(ptep));
	ADD_STATS(pte_update_batched, paravirt_get_lazy_mode() == PARAVIRT_LAZY_MMU);

#ifdef CONFIG_X86_PAE
	ptep->pte_high = pte.pte_high;
	smp_wmb();
	ptep->pte_low = pte.pte_low;
#else
	*ptep = pte;
#endif
}

#ifdef CONFIG_X86_PAE
void xen_set_pte_atomic(pte_t *ptep, pte_t pte)
{
	set_64bit((u64 *)ptep, native_pte_val(pte));
}

void xen_pte_clear(struct mm_struct *mm, unsigned long addr, pte_t *ptep)
{
	ptep->pte_low = 0;
	smp_wmb();		/* make sure low gets written first */
	ptep->pte_high = 0;
}

void xen_pmd_clear(pmd_t *pmdp)
{
	set_pmd(pmdp, __pmd(0));
}
#endif	/* CONFIG_X86_PAE */

pmd_t xen_make_pmd(pmdval_t pmd)
{
	pmd = pte_pfn_to_mfn(pmd);
	return native_make_pmd(pmd);
}
PV_CALLEE_SAVE_REGS_THUNK(xen_make_pmd);

#if PAGETABLE_LEVELS == 4
pudval_t xen_pud_val(pud_t pud)
{
	return pte_mfn_to_pfn(pud.pud);
}
PV_CALLEE_SAVE_REGS_THUNK(xen_pud_val);

pud_t xen_make_pud(pudval_t pud)
{
	pud = pte_pfn_to_mfn(pud);

	return native_make_pud(pud);
}
PV_CALLEE_SAVE_REGS_THUNK(xen_make_pud);

pgd_t *xen_get_user_pgd(pgd_t *pgd)
{
	pgd_t *pgd_page = (pgd_t *)(((unsigned long)pgd) & PAGE_MASK);
	unsigned offset = pgd - pgd_page;
	pgd_t *user_ptr = NULL;

	if (offset < pgd_index(USER_LIMIT)) {
		struct page *page = virt_to_page(pgd_page);
		user_ptr = (pgd_t *)page->private;
		if (user_ptr)
			user_ptr += offset;
	}

	return user_ptr;
}

static void __xen_set_pgd_hyper(pgd_t *ptr, pgd_t val)
{
	struct mmu_update u;

	u.ptr = virt_to_machine(ptr).maddr;
	u.val = pgd_val_ma(val);
	xen_extend_mmu_update(&u);
}

/*
 * Raw hypercall-based set_pgd, intended for in early boot before
 * there's a page structure.  This implies:
 *  1. The only existing pagetable is the kernel's
 *  2. It is always pinned
 *  3. It has no user pagetable attached to it
 */
void __init xen_set_pgd_hyper(pgd_t *ptr, pgd_t val)
{
	preempt_disable();

	xen_mc_batch();

	__xen_set_pgd_hyper(ptr, val);

	xen_mc_issue(PARAVIRT_LAZY_MMU);

	preempt_enable();
}

void xen_set_pgd(pgd_t *ptr, pgd_t val)
{
	pgd_t *user_ptr = xen_get_user_pgd(ptr);

	ADD_STATS(pgd_update, 1);

	/* If page is not pinned, we can just update the entry
	   directly */
	if (!xen_page_pinned(ptr)) {
		*ptr = val;
		if (user_ptr) {
			WARN_ON(xen_page_pinned(user_ptr));
			*user_ptr = val;
		}
		return;
	}

	ADD_STATS(pgd_update_pinned, 1);
	ADD_STATS(pgd_update_batched, paravirt_get_lazy_mode() == PARAVIRT_LAZY_MMU);

	/* If it's pinned, then we can at least batch the kernel and
	   user updates together. */
	xen_mc_batch();

	__xen_set_pgd_hyper(ptr, val);
	if (user_ptr)
		__xen_set_pgd_hyper(user_ptr, val);

	xen_mc_issue(PARAVIRT_LAZY_MMU);
}
#endif	/* PAGETABLE_LEVELS == 4 */

/*
 * (Yet another) pagetable walker.  This one is intended for pinning a
 * pagetable.  This means that it walks a pagetable and calls the
 * callback function on each page it finds making up the page table,
 * at every level.  It walks the entire pagetable, but it only bothers
 * pinning pte pages which are below limit.  In the normal case this
 * will be STACK_TOP_MAX, but at boot we need to pin up to
 * FIXADDR_TOP.
 *
 * For 32-bit the important bit is that we don't pin beyond there,
 * because then we start getting into Xen's ptes.
 *
 * For 64-bit, we must skip the Xen hole in the middle of the address
 * space, just after the big x86-64 virtual hole.
 */
static int __xen_pgd_walk(struct mm_struct *mm, pgd_t *pgd,
			  int (*func)(struct mm_struct *mm, struct page *,
				      enum pt_level),
			  unsigned long limit)
{
	int flush = 0;
	unsigned hole_low, hole_high;
	unsigned pgdidx_limit, pudidx_limit, pmdidx_limit;
	unsigned pgdidx, pudidx, pmdidx;

	/* The limit is the last byte to be touched */
	limit--;
	BUG_ON(limit >= FIXADDR_TOP);

	if (xen_feature(XENFEAT_auto_translated_physmap))
		return 0;

	/*
	 * 64-bit has a great big hole in the middle of the address
	 * space, which contains the Xen mappings.  On 32-bit these
	 * will end up making a zero-sized hole and so is a no-op.
	 */
	hole_low = pgd_index(USER_LIMIT);
	hole_high = pgd_index(PAGE_OFFSET);

	pgdidx_limit = pgd_index(limit);
#if PTRS_PER_PUD > 1
	pudidx_limit = pud_index(limit);
#else
	pudidx_limit = 0;
#endif
#if PTRS_PER_PMD > 1
	pmdidx_limit = pmd_index(limit);
#else
	pmdidx_limit = 0;
#endif

	for (pgdidx = 0; pgdidx <= pgdidx_limit; pgdidx++) {
		pud_t *pud;

		if (pgdidx >= hole_low && pgdidx < hole_high)
			continue;

		if (!pgd_val(pgd[pgdidx]))
			continue;

		pud = pud_offset(&pgd[pgdidx], 0);

		if (PTRS_PER_PUD > 1) /* not folded */
			flush |= (*func)(mm, virt_to_page(pud), PT_PUD);

		for (pudidx = 0; pudidx < PTRS_PER_PUD; pudidx++) {
			pmd_t *pmd;

			if (pgdidx == pgdidx_limit &&
			    pudidx > pudidx_limit)
				goto out;

			if (pud_none(pud[pudidx]))
				continue;

			pmd = pmd_offset(&pud[pudidx], 0);

			if (PTRS_PER_PMD > 1) /* not folded */
				flush |= (*func)(mm, virt_to_page(pmd), PT_PMD);

			for (pmdidx = 0; pmdidx < PTRS_PER_PMD; pmdidx++) {
				struct page *pte;

				if (pgdidx == pgdidx_limit &&
				    pudidx == pudidx_limit &&
				    pmdidx > pmdidx_limit)
					goto out;

				if (pmd_none(pmd[pmdidx]))
					continue;

				pte = pmd_page(pmd[pmdidx]);
				flush |= (*func)(mm, pte, PT_PTE);
			}
		}
	}

out:
	/* Do the top level last, so that the callbacks can use it as
	   a cue to do final things like tlb flushes. */
	flush |= (*func)(mm, virt_to_page(pgd), PT_PGD);

	return flush;
}

static int xen_pgd_walk(struct mm_struct *mm,
			int (*func)(struct mm_struct *mm, struct page *,
				    enum pt_level),
			unsigned long limit)
{
	return __xen_pgd_walk(mm, mm->pgd, func, limit);
}

/* If we're using split pte locks, then take the page's lock and
   return a pointer to it.  Otherwise return NULL. */
static spinlock_t *xen_pte_lock(struct page *page, struct mm_struct *mm)
{
	spinlock_t *ptl = NULL;

#if USE_SPLIT_PTLOCKS
	ptl = __pte_lockptr(page);
	spin_lock_nest_lock(ptl, &mm->page_table_lock);
#endif

	return ptl;
}

static void xen_pte_unlock(void *v)
{
	spinlock_t *ptl = v;
	spin_unlock(ptl);
}

static void xen_do_pin(unsigned level, unsigned long pfn)
{
	struct mmuext_op *op;
	struct multicall_space mcs;

	mcs = __xen_mc_entry(sizeof(*op));
	op = mcs.args;
	op->cmd = level;
	op->arg1.mfn = pfn_to_mfn(pfn);
	MULTI_mmuext_op(mcs.mc, op, 1, NULL, DOMID_SELF);
}

static int xen_pin_page(struct mm_struct *mm, struct page *page,
			enum pt_level level)
{
	unsigned pgfl = TestSetPagePinned(page);
	int flush;

	if (pgfl)
		flush = 0;		/* already pinned */
	else if (PageHighMem(page))
		/* kmaps need flushing if we found an unpinned
		   highpage */
		flush = 1;
	else {
		void *pt = lowmem_page_address(page);
		unsigned long pfn = page_to_pfn(page);
		struct multicall_space mcs = __xen_mc_entry(0);
		spinlock_t *ptl;

		flush = 0;

		/*
		 * We need to hold the pagetable lock between the time
		 * we make the pagetable RO and when we actually pin
		 * it.  If we don't, then other users may come in and
		 * attempt to update the pagetable by writing it,
		 * which will fail because the memory is RO but not
		 * pinned, so Xen won't do the trap'n'emulate.
		 *
		 * If we're using split pte locks, we can't hold the
		 * entire pagetable's worth of locks during the
		 * traverse, because we may wrap the preempt count (8
		 * bits).  The solution is to mark RO and pin each PTE
		 * page while holding the lock.  This means the number
		 * of locks we end up holding is never more than a
		 * batch size (~32 entries, at present).
		 *
		 * If we're not using split pte locks, we needn't pin
		 * the PTE pages independently, because we're
		 * protected by the overall pagetable lock.
		 */
		ptl = NULL;
		if (level == PT_PTE)
			ptl = xen_pte_lock(page, mm);

		MULTI_update_va_mapping(mcs.mc, (unsigned long)pt,
					pfn_pte(pfn, PAGE_KERNEL_RO),
					level == PT_PGD ? UVMF_TLB_FLUSH : 0);

		if (ptl) {
			xen_do_pin(MMUEXT_PIN_L1_TABLE, pfn);

			/* Queue a deferred unlock for when this batch
			   is completed. */
			xen_mc_callback(xen_pte_unlock, ptl);
		}
	}

	return flush;
}

/* This is called just after a mm has been created, but it has not
   been used yet.  We need to make sure that its pagetable is all
   read-only, and can be pinned. */
static void __xen_pgd_pin(struct mm_struct *mm, pgd_t *pgd)
{
	xen_mc_batch();

	if (__xen_pgd_walk(mm, pgd, xen_pin_page, USER_LIMIT)) {
		/* re-enable interrupts for flushing */
		xen_mc_issue(0);

		kmap_flush_unused();

		xen_mc_batch();
	}

#ifdef CONFIG_X86_64
	{
		pgd_t *user_pgd = xen_get_user_pgd(pgd);

		xen_do_pin(MMUEXT_PIN_L4_TABLE, PFN_DOWN(__pa(pgd)));

		if (user_pgd) {
			xen_pin_page(mm, virt_to_page(user_pgd), PT_PGD);
			xen_do_pin(MMUEXT_PIN_L4_TABLE,
				   PFN_DOWN(__pa(user_pgd)));
		}
	}
#else /* CONFIG_X86_32 */
#ifdef CONFIG_X86_PAE
	/* Need to make sure unshared kernel PMD is pinnable */
	xen_pin_page(mm, pgd_page(pgd[pgd_index(TASK_SIZE)]),
		     PT_PMD);
#endif
	xen_do_pin(MMUEXT_PIN_L3_TABLE, PFN_DOWN(__pa(pgd)));
#endif /* CONFIG_X86_64 */
	xen_mc_issue(0);
}

static void xen_pgd_pin(struct mm_struct *mm)
{
	__xen_pgd_pin(mm, mm->pgd);
}

/*
 * On save, we need to pin all pagetables to make sure they get their
 * mfns turned into pfns.  Search the list for any unpinned pgds and pin
 * them (unpinned pgds are not currently in use, probably because the
 * process is under construction or destruction).
 *
 * Expected to be called in stop_machine() ("equivalent to taking
 * every spinlock in the system"), so the locking doesn't really
 * matter all that much.
 */
void xen_mm_pin_all(void)
{
	unsigned long flags;
	struct page *page;

	spin_lock_irqsave(&pgd_lock, flags);

	list_for_each_entry(page, &pgd_list, lru) {
		if (!PagePinned(page)) {
			__xen_pgd_pin(&init_mm, (pgd_t *)page_address(page));
			SetPageSavePinned(page);
		}
	}

	spin_unlock_irqrestore(&pgd_lock, flags);
}

/*
 * The init_mm pagetable is really pinned as soon as its created, but
 * that's before we have page structures to store the bits.  So do all
 * the book-keeping now.
 */
static __init int xen_mark_pinned(struct mm_struct *mm, struct page *page,
				  enum pt_level level)
{
	SetPagePinned(page);
	return 0;
}

static void __init xen_mark_init_mm_pinned(void)
{
	xen_pgd_walk(&init_mm, xen_mark_pinned, FIXADDR_TOP);
}

static int xen_unpin_page(struct mm_struct *mm, struct page *page,
			  enum pt_level level)
{
	unsigned pgfl = TestClearPagePinned(page);

	if (pgfl && !PageHighMem(page)) {
		void *pt = lowmem_page_address(page);
		unsigned long pfn = page_to_pfn(page);
		spinlock_t *ptl = NULL;
		struct multicall_space mcs;

		/*
		 * Do the converse to pin_page.  If we're using split
		 * pte locks, we must be holding the lock for while
		 * the pte page is unpinned but still RO to prevent
		 * concurrent updates from seeing it in this
		 * partially-pinned state.
		 */
		if (level == PT_PTE) {
			ptl = xen_pte_lock(page, mm);

			if (ptl)
				xen_do_pin(MMUEXT_UNPIN_TABLE, pfn);
		}

		mcs = __xen_mc_entry(0);

		MULTI_update_va_mapping(mcs.mc, (unsigned long)pt,
					pfn_pte(pfn, PAGE_KERNEL),
					level == PT_PGD ? UVMF_TLB_FLUSH : 0);

		if (ptl) {
			/* unlock when batch completed */
			xen_mc_callback(xen_pte_unlock, ptl);
		}
	}

	return 0;		/* never need to flush on unpin */
}

/* Release a pagetables pages back as normal RW */
static void __xen_pgd_unpin(struct mm_struct *mm, pgd_t *pgd)
{
	xen_mc_batch();

	xen_do_pin(MMUEXT_UNPIN_TABLE, PFN_DOWN(__pa(pgd)));

#ifdef CONFIG_X86_64
	{
		pgd_t *user_pgd = xen_get_user_pgd(pgd);

		if (user_pgd) {
			xen_do_pin(MMUEXT_UNPIN_TABLE,
				   PFN_DOWN(__pa(user_pgd)));
			xen_unpin_page(mm, virt_to_page(user_pgd), PT_PGD);
		}
	}
#endif

#ifdef CONFIG_X86_PAE
	/* Need to make sure unshared kernel PMD is unpinned */
	xen_unpin_page(mm, pgd_page(pgd[pgd_index(TASK_SIZE)]),
		       PT_PMD);
#endif

	__xen_pgd_walk(mm, pgd, xen_unpin_page, USER_LIMIT);

	xen_mc_issue(0);
}

static void xen_pgd_unpin(struct mm_struct *mm)
{
	__xen_pgd_unpin(mm, mm->pgd);
}

/*
 * On resume, undo any pinning done at save, so that the rest of the
 * kernel doesn't see any unexpected pinned pagetables.
 */
void xen_mm_unpin_all(void)
{
	unsigned long flags;
	struct page *page;

	spin_lock_irqsave(&pgd_lock, flags);

	list_for_each_entry(page, &pgd_list, lru) {
		if (PageSavePinned(page)) {
			BUG_ON(!PagePinned(page));
			__xen_pgd_unpin(&init_mm, (pgd_t *)page_address(page));
			ClearPageSavePinned(page);
		}
	}

	spin_unlock_irqrestore(&pgd_lock, flags);
}

void xen_activate_mm(struct mm_struct *prev, struct mm_struct *next)
{
	spin_lock(&next->page_table_lock);
	xen_pgd_pin(next);
	spin_unlock(&next->page_table_lock);
}

void xen_dup_mmap(struct mm_struct *oldmm, struct mm_struct *mm)
{
	spin_lock(&mm->page_table_lock);
	xen_pgd_pin(mm);
	spin_unlock(&mm->page_table_lock);
}


#ifdef CONFIG_SMP
/* Another cpu may still have their %cr3 pointing at the pagetable, so
   we need to repoint it somewhere else before we can unpin it. */
static void drop_other_mm_ref(void *info)
{
	struct mm_struct *mm = info;
	struct mm_struct *active_mm;

	active_mm = percpu_read(cpu_tlbstate.active_mm);

	if (active_mm == mm && percpu_read(cpu_tlbstate.state) != TLBSTATE_OK)
		leave_mm(smp_processor_id());

	/* If this cpu still has a stale cr3 reference, then make sure
	   it has been flushed. */
	if (percpu_read(xen_current_cr3) == __pa(mm->pgd))
		load_cr3(swapper_pg_dir);
}

static void xen_drop_mm_ref(struct mm_struct *mm)
{
	cpumask_var_t mask;
	unsigned cpu;

	if (current->active_mm == mm) {
		if (current->mm == mm)
			load_cr3(swapper_pg_dir);
		else
			leave_mm(smp_processor_id());
	}

	/* Get the "official" set of cpus referring to our pagetable. */
	if (!alloc_cpumask_var(&mask, GFP_ATOMIC)) {
		for_each_online_cpu(cpu) {
			if (!cpumask_test_cpu(cpu, mm_cpumask(mm))
			    && per_cpu(xen_current_cr3, cpu) != __pa(mm->pgd))
				continue;
			smp_call_function_single(cpu, drop_other_mm_ref, mm, 1);
		}
		return;
	}
	cpumask_copy(mask, mm_cpumask(mm));

	/* It's possible that a vcpu may have a stale reference to our
	   cr3, because its in lazy mode, and it hasn't yet flushed
	   its set of pending hypercalls yet.  In this case, we can
	   look at its actual current cr3 value, and force it to flush
	   if needed. */
	for_each_online_cpu(cpu) {
		if (per_cpu(xen_current_cr3, cpu) == __pa(mm->pgd))
			cpumask_set_cpu(cpu, mask);
	}

	if (!cpumask_empty(mask))
		smp_call_function_many(mask, drop_other_mm_ref, mm, 1);
	free_cpumask_var(mask);
}
#else
static void xen_drop_mm_ref(struct mm_struct *mm)
{
	if (current->active_mm == mm)
		load_cr3(swapper_pg_dir);
}
#endif

/*
 * While a process runs, Xen pins its pagetables, which means that the
 * hypervisor forces it to be read-only, and it controls all updates
 * to it.  This means that all pagetable updates have to go via the
 * hypervisor, which is moderately expensive.
 *
 * Since we're pulling the pagetable down, we switch to use init_mm,
 * unpin old process pagetable and mark it all read-write, which
 * allows further operations on it to be simple memory accesses.
 *
 * The only subtle point is that another CPU may be still using the
 * pagetable because of lazy tlb flushing.  This means we need need to
 * switch all CPUs off this pagetable before we can unpin it.
 */
void xen_exit_mmap(struct mm_struct *mm)
{
	get_cpu();		/* make sure we don't move around */
	xen_drop_mm_ref(mm);
	put_cpu();

	spin_lock(&mm->page_table_lock);

	/* pgd may not be pinned in the error exit path of execve */
	if (xen_page_pinned(mm->pgd) && !mm->context.has_foreign_mappings)
		xen_pgd_unpin(mm);

	spin_unlock(&mm->page_table_lock);
}

static __init void xen_pagetable_setup_start(pgd_t *base)
{
}

static void xen_post_allocator_init(void);

static __init void xen_pagetable_setup_done(pgd_t *base)
{
	xen_setup_shared_info();
	xen_post_allocator_init();
}

static void xen_write_cr2(unsigned long cr2)
{
	percpu_read(xen_vcpu)->arch.cr2 = cr2;
}

static unsigned long xen_read_cr2(void)
{
	return percpu_read(xen_vcpu)->arch.cr2;
}

unsigned long xen_read_cr2_direct(void)
{
	return percpu_read(xen_vcpu_info.arch.cr2);
}

static void xen_flush_tlb(void)
{
	struct mmuext_op *op;
	struct multicall_space mcs;

	preempt_disable();

	mcs = xen_mc_entry(sizeof(*op));

	op = mcs.args;
	op->cmd = MMUEXT_TLB_FLUSH_LOCAL;
	MULTI_mmuext_op(mcs.mc, op, 1, NULL, DOMID_SELF);

	xen_mc_issue(PARAVIRT_LAZY_MMU);

	preempt_enable();
}

static void xen_flush_tlb_single(unsigned long addr)
{
	struct mmuext_op *op;
	struct multicall_space mcs;

	preempt_disable();

	mcs = xen_mc_entry(sizeof(*op));
	op = mcs.args;
	op->cmd = MMUEXT_INVLPG_LOCAL;
	op->arg1.linear_addr = addr & PAGE_MASK;
	MULTI_mmuext_op(mcs.mc, op, 1, NULL, DOMID_SELF);

	xen_mc_issue(PARAVIRT_LAZY_MMU);

	preempt_enable();
}

/*
 * Flush tlb on other cpus.  Xen can do this via a single hypercall
 * rather than explicit IPIs, which has the nice property of avoiding
 * any cpus which don't actually have dirty tlbs.  Unfortunately it
 * doesn't give us an opportunity to kick out cpus which are in lazy
 * tlb state, so we may end up reflushing some cpus unnecessarily.
 */
static void xen_flush_tlb_others(const struct cpumask *cpus,
				 struct mm_struct *mm, unsigned long va)
{
	struct {
		struct mmuext_op op;
		DECLARE_BITMAP(mask, num_processors);
	} *args;
	struct multicall_space mcs;

	if (cpumask_empty(cpus))
		return;		/* nothing to do */

	mcs = xen_mc_entry(sizeof(*args));
	args = mcs.args;
	args->op.arg2.vcpumask = to_cpumask(args->mask);

	/* Remove us, and any offline CPUS. */
	cpumask_and(to_cpumask(args->mask), cpus, cpu_online_mask);
	cpumask_clear_cpu(smp_processor_id(), to_cpumask(args->mask));

	if (va == TLB_FLUSH_ALL) {
		args->op.cmd = MMUEXT_TLB_FLUSH_MULTI;
	} else {
		args->op.cmd = MMUEXT_INVLPG_MULTI;
		args->op.arg1.linear_addr = va;
	}

	MULTI_mmuext_op(mcs.mc, &args->op, 1, NULL, DOMID_SELF);

	xen_mc_issue(PARAVIRT_LAZY_MMU);
}

static unsigned long xen_read_cr3(void)
{
	return percpu_read(xen_cr3);
}

static void set_current_cr3(void *v)
{
	percpu_write(xen_current_cr3, (unsigned long)v);
}

static void __xen_write_cr3(bool kernel, unsigned long cr3)
{
	struct mmuext_op *op;
	struct multicall_space mcs;
	unsigned long mfn;

	if (cr3)
		mfn = pfn_to_mfn(PFN_DOWN(cr3));
	else
		mfn = 0;

	WARN_ON(mfn == 0 && kernel);

	mcs = __xen_mc_entry(sizeof(*op));

	op = mcs.args;
	op->cmd = kernel ? MMUEXT_NEW_BASEPTR : MMUEXT_NEW_USER_BASEPTR;
	op->arg1.mfn = mfn;

	MULTI_mmuext_op(mcs.mc, op, 1, NULL, DOMID_SELF);

	if (kernel) {
		percpu_write(xen_cr3, cr3);

		/* Update xen_current_cr3 once the batch has actually
		   been submitted. */
		xen_mc_callback(set_current_cr3, (void *)cr3);
	}
}

static void xen_write_cr3(unsigned long cr3)
{
	BUG_ON(preemptible());

	xen_mc_batch();  /* disables interrupts */

	/* Update while interrupts are disabled, so its atomic with
	   respect to ipis */
	percpu_write(xen_cr3, cr3);

	__xen_write_cr3(true, cr3);

#ifdef CONFIG_X86_64
	{
		pgd_t *user_pgd = xen_get_user_pgd(__va(cr3));
		if (user_pgd)
			__xen_write_cr3(false, __pa(user_pgd));
		else
			__xen_write_cr3(false, 0);
	}
#endif

	xen_mc_issue(PARAVIRT_LAZY_CPU);  /* interrupts restored */
}

static int xen_pgd_alloc(struct mm_struct *mm)
{
	pgd_t *pgd = mm->pgd;
	int ret = 0;

	BUG_ON(PagePinned(virt_to_page(pgd)));

#ifdef CONFIG_X86_64
	{
		struct page *page = virt_to_page(pgd);
		pgd_t *user_pgd;

		BUG_ON(page->private != 0);

		ret = -ENOMEM;

		user_pgd = (pgd_t *)__get_free_page(GFP_KERNEL | __GFP_ZERO);
		page->private = (unsigned long)user_pgd;

		if (user_pgd != NULL) {
			user_pgd[pgd_index(VSYSCALL_START)] =
				__pgd(__pa(level3_user_vsyscall) | _PAGE_TABLE);
			ret = 0;
		}

		BUG_ON(PagePinned(virt_to_page(xen_get_user_pgd(pgd))));
	}
#endif

	return ret;
}

void xen_late_unpin_pgd(struct mm_struct *mm, pgd_t *pgd)
{
	if (xen_page_pinned(pgd))
		__xen_pgd_unpin(mm, pgd);

}

static void xen_pgd_free(struct mm_struct *mm, pgd_t *pgd)
{
#ifdef CONFIG_X86_64
	pgd_t *user_pgd = xen_get_user_pgd(pgd);

	if (user_pgd)
		free_page((unsigned long)user_pgd);
#endif
}

#ifdef CONFIG_HIGHPTE
static void *xen_kmap_atomic_pte(struct page *page, enum km_type type)
{
	pgprot_t prot = PAGE_KERNEL;

	/*
	 * We disable highmem allocations for page tables so we should never
	 * see any calls to kmap_atomic_pte on a highmem page.
	 */
	BUG_ON(PageHighMem(page));

	if (PagePinned(page))
		prot = PAGE_KERNEL_RO;

	return kmap_atomic_prot(page, type, prot);
}
#endif

static __init pte_t mask_rw_pte(pte_t *ptep, pte_t pte)
{
	unsigned long pfn = pte_pfn(pte);
	pte_t oldpte = *ptep;

	if (pte_flags(oldpte) & _PAGE_PRESENT) {
		/* Don't allow existing IO mappings to be overridden */
		if (pte_flags(oldpte) & _PAGE_IOMAP)
			pte = oldpte;

		/* Don't allow _PAGE_RW to be set on existing pte */
		pte = __pte_ma(((pte_val_ma(*ptep) & _PAGE_RW) | ~_PAGE_RW) &
			       pte_val_ma(pte));
	}

	/*
	 * If the new pfn is within the range of the newly allocated
	 * kernel pagetable, and it isn't being mapped into an
	 * early_ioremap fixmap slot, make sure it is RO.
	 */
	if (!is_early_ioremap_ptep(ptep) &&
	    pfn >= e820_table_start && pfn < e820_table_end)
		pte = pte_wrprotect(pte);

	return pte;
}

/* Init-time set_pte while constructing initial pagetables, which
   doesn't allow RO pagetable pages to be remapped RW */
static __init void xen_set_pte_init(pte_t *ptep, pte_t pte)
{
	pte = mask_rw_pte(ptep, pte);

	xen_set_pte(ptep, pte);
}

static void pin_pagetable_pfn(unsigned cmd, unsigned long pfn)
{
	struct mmuext_op op;
	op.cmd = cmd;
	op.arg1.mfn = pfn_to_mfn(pfn);
	if (HYPERVISOR_mmuext_op(&op, 1, NULL, DOMID_SELF))
		BUG();
}

/* Early in boot, while setting up the initial pagetable, assume
   everything is pinned. */
static __init void xen_alloc_pte_init(struct mm_struct *mm, unsigned long pfn)
{
#ifdef CONFIG_FLATMEM
	BUG_ON(mem_map);	/* should only be used early */
#endif
	make_lowmem_page_readonly(__va(PFN_PHYS(pfn)));
	pin_pagetable_pfn(MMUEXT_PIN_L1_TABLE, pfn);
}

/* Used for pmd and pud */
static __init void xen_alloc_pmd_init(struct mm_struct *mm, unsigned long pfn)
{
#ifdef CONFIG_FLATMEM
	BUG_ON(mem_map);	/* should only be used early */
#endif
	make_lowmem_page_readonly(__va(PFN_PHYS(pfn)));
}

/* Early release_pte assumes that all pts are pinned, since there's
   only init_mm and anything attached to that is pinned. */
static __init void xen_release_pte_init(unsigned long pfn)
{
	pin_pagetable_pfn(MMUEXT_UNPIN_TABLE, pfn);
	make_lowmem_page_readwrite(__va(PFN_PHYS(pfn)));
}

static __init void xen_release_pmd_init(unsigned long pfn)
{
	make_lowmem_page_readwrite(__va(PFN_PHYS(pfn)));
}

/* This needs to make sure the new pte page is pinned iff its being
   attached to a pinned pagetable. */
static void xen_alloc_ptpage(struct mm_struct *mm, unsigned long pfn, unsigned level)
{
	struct page *page = pfn_to_page(pfn);

	if (PagePinned(virt_to_page(mm->pgd))) {
		SetPagePinned(page);

		if (!PageHighMem(page)) {
			make_lowmem_page_readonly(__va(PFN_PHYS((unsigned long)pfn)));
			if (level == PT_PTE && USE_SPLIT_PTLOCKS)
				pin_pagetable_pfn(MMUEXT_PIN_L1_TABLE, pfn);
		} else {
			/* make sure there are no stray mappings of
			   this page */
			kmap_flush_unused();
		}
	}
}

static void xen_alloc_pte(struct mm_struct *mm, unsigned long pfn)
{
	xen_alloc_ptpage(mm, pfn, PT_PTE);
}

static void xen_alloc_pmd(struct mm_struct *mm, unsigned long pfn)
{
	xen_alloc_ptpage(mm, pfn, PT_PMD);
}

/* This should never happen until we're OK to use struct page */
static void xen_release_ptpage(unsigned long pfn, unsigned level)
{
	struct page *page = pfn_to_page(pfn);

	if (PagePinned(page)) {
		if (!PageHighMem(page)) {
			if (level == PT_PTE && USE_SPLIT_PTLOCKS)
				pin_pagetable_pfn(MMUEXT_UNPIN_TABLE, pfn);
			make_lowmem_page_readwrite(__va(PFN_PHYS(pfn)));
		}
		ClearPagePinned(page);
	}
}

static void xen_release_pte(unsigned long pfn)
{
	xen_release_ptpage(pfn, PT_PTE);
}

static void xen_release_pmd(unsigned long pfn)
{
	xen_release_ptpage(pfn, PT_PMD);
}

#if PAGETABLE_LEVELS == 4
static void xen_alloc_pud(struct mm_struct *mm, unsigned long pfn)
{
	xen_alloc_ptpage(mm, pfn, PT_PUD);
}

static void xen_release_pud(unsigned long pfn)
{
	xen_release_ptpage(pfn, PT_PUD);
}
#endif

void __init xen_reserve_top(void)
{
#ifdef CONFIG_X86_32
	unsigned long top = HYPERVISOR_VIRT_START;
	struct xen_platform_parameters pp;

	if (HYPERVISOR_xen_version(XENVER_platform_parameters, &pp) == 0)
		top = pp.virt_start;

	reserve_top_address(-top);
#endif	/* CONFIG_X86_32 */
}

/*
 * Like __va(), but returns address in the kernel mapping (which is
 * all we have until the physical memory mapping has been set up.
 */
static void *__ka(phys_addr_t paddr)
{
#ifdef CONFIG_X86_64
	return (void *)(paddr + __START_KERNEL_map);
#else
	return __va(paddr);
#endif
}

/* Convert a machine address to physical address */
static unsigned long m2p(phys_addr_t maddr)
{
	phys_addr_t paddr;

	maddr &= PTE_PFN_MASK;
	paddr = mfn_to_pfn(maddr >> PAGE_SHIFT) << PAGE_SHIFT;

	return paddr;
}

/* Convert a machine address to kernel virtual */
static void *m2v(phys_addr_t maddr)
{
	return __ka(m2p(maddr));
}

/* Set the page permissions on an identity-mapped pages */
static void set_page_prot(void *addr, pgprot_t prot)
{
	unsigned long pfn = __pa(addr) >> PAGE_SHIFT;
	pte_t pte = pfn_pte(pfn, prot);

	if (HYPERVISOR_update_va_mapping((unsigned long)addr, pte, 0))
		BUG();
}

static __init void xen_map_identity_early(pmd_t *pmd, unsigned long max_pfn)
{
	unsigned pmdidx, pteidx;
	unsigned ident_pte;
	unsigned long pfn;

	level1_ident_pgt = extend_brk(sizeof(pte_t) * LEVEL1_IDENT_ENTRIES,
				      PAGE_SIZE);

	ident_pte = 0;
	pfn = 0;
	for (pmdidx = 0; pmdidx < PTRS_PER_PMD && pfn < max_pfn; pmdidx++) {
		pte_t *pte_page;

		/* Reuse or allocate a page of ptes */
		if (pmd_present(pmd[pmdidx]))
			pte_page = m2v(pmd[pmdidx].pmd);
		else {
			/* Check for free pte pages */
			if (ident_pte == LEVEL1_IDENT_ENTRIES)
				break;

			pte_page = &level1_ident_pgt[ident_pte];
			ident_pte += PTRS_PER_PTE;

			pmd[pmdidx] = __pmd(__pa(pte_page) | _PAGE_TABLE);
		}

		/* Install mappings */
		for (pteidx = 0; pteidx < PTRS_PER_PTE; pteidx++, pfn++) {
			pte_t pte;

			if (!pte_none(pte_page[pteidx]))
				continue;

			pte = pfn_pte(pfn, PAGE_KERNEL_EXEC);
			pte_page[pteidx] = pte;
		}
	}

	for (pteidx = 0; pteidx < ident_pte; pteidx += PTRS_PER_PTE)
		set_page_prot(&level1_ident_pgt[pteidx], PAGE_KERNEL_RO);

	set_page_prot(pmd, PAGE_KERNEL_RO);
}

void __init xen_setup_machphys_mapping(void)
{
	struct xen_machphys_mapping mapping;
	unsigned long machine_to_phys_nr_ents;

	if (HYPERVISOR_memory_op(XENMEM_machphys_mapping, &mapping) == 0) {
		machine_to_phys_mapping = (unsigned long *)mapping.v_start;
		machine_to_phys_nr_ents = mapping.max_mfn + 1;
	} else {
		machine_to_phys_nr_ents = MACH2PHYS_NR_ENTRIES;
	}
	machine_to_phys_order = fls(machine_to_phys_nr_ents - 1);
}

#ifdef CONFIG_X86_64
static void convert_pfn_mfn(void *v)
{
	pte_t *pte = v;
	int i;

	/* All levels are converted the same way, so just treat them
	   as ptes. */
	for (i = 0; i < PTRS_PER_PTE; i++)
		pte[i] = xen_make_pte(pte[i].pte);
}

/*
 * Set up the inital kernel pagetable.
 *
 * We can construct this by grafting the Xen provided pagetable into
 * head_64.S's preconstructed pagetables.  We copy the Xen L2's into
 * level2_ident_pgt, level2_kernel_pgt and level2_fixmap_pgt.  This
 * means that only the kernel has a physical mapping to start with -
 * but that's enough to get __va working.  We need to fill in the rest
 * of the physical mapping once some sort of allocator has been set
 * up.
 */
__init pgd_t *xen_setup_kernel_pagetable(pgd_t *pgd,
					 unsigned long max_pfn)
{
	pud_t *l3;
	pmd_t *l2;

	/* max_pfn_mapped is the last pfn mapped in the initial memory
	 * mappings. Considering that on Xen after the kernel mappings we
	 * have the mappings of some pages that don't exist in pfn space, we
	 * set max_pfn_mapped to the last real pfn mapped. */
	max_pfn_mapped = PFN_DOWN(__pa(xen_start_info->mfn_list));

	/* Zap identity mapping */
	init_level4_pgt[0] = __pgd(0);

	/* Pre-constructed entries are in pfn, so convert to mfn */
	convert_pfn_mfn(init_level4_pgt);
	convert_pfn_mfn(level3_ident_pgt);
	convert_pfn_mfn(level3_kernel_pgt);

	l3 = m2v(pgd[pgd_index(__START_KERNEL_map)].pgd);
	l2 = m2v(l3[pud_index(__START_KERNEL_map)].pud);

	memcpy(level2_ident_pgt, l2, sizeof(pmd_t) * PTRS_PER_PMD);
	memcpy(level2_kernel_pgt, l2, sizeof(pmd_t) * PTRS_PER_PMD);

	l3 = m2v(pgd[pgd_index(__START_KERNEL_map + PMD_SIZE)].pgd);
	l2 = m2v(l3[pud_index(__START_KERNEL_map + PMD_SIZE)].pud);
	memcpy(level2_fixmap_pgt, l2, sizeof(pmd_t) * PTRS_PER_PMD);

	/* Set up identity map */
	xen_map_identity_early(level2_ident_pgt, max_pfn);

	/* Make pagetable pieces RO */
	set_page_prot(init_level4_pgt, PAGE_KERNEL_RO);
	set_page_prot(level3_ident_pgt, PAGE_KERNEL_RO);
	set_page_prot(level3_kernel_pgt, PAGE_KERNEL_RO);
	set_page_prot(level3_user_vsyscall, PAGE_KERNEL_RO);
	set_page_prot(level2_kernel_pgt, PAGE_KERNEL_RO);
	set_page_prot(level2_fixmap_pgt, PAGE_KERNEL_RO);

	/* Pin down new L4 */
	pin_pagetable_pfn(MMUEXT_PIN_L4_TABLE,
			  PFN_DOWN(__pa_symbol(init_level4_pgt)));

	/* Unpin Xen-provided one */
	pin_pagetable_pfn(MMUEXT_UNPIN_TABLE, PFN_DOWN(__pa(pgd)));

	/* Switch over */
	pgd = init_level4_pgt;

	/*
	 * At this stage there can be no user pgd, and no page
	 * structure to attach it to, so make sure we just set kernel
	 * pgd.
	 */
	xen_mc_batch();
	__xen_write_cr3(true, __pa(pgd));
	xen_mc_issue(PARAVIRT_LAZY_CPU);

	reserve_early(__pa(xen_start_info->pt_base),
		      __pa(xen_start_info->pt_base +
			   xen_start_info->nr_pt_frames * PAGE_SIZE),
		      "XEN PAGETABLES");

	return pgd;
}
#else	/* !CONFIG_X86_64 */
static RESERVE_BRK_ARRAY(pmd_t, level2_kernel_pgt, PTRS_PER_PMD);

__init pgd_t *xen_setup_kernel_pagetable(pgd_t *pgd,
					 unsigned long max_pfn)
{
	pmd_t *kernel_pmd;
	int i;

	level2_kernel_pgt = extend_brk(sizeof(pmd_t) * PTRS_PER_PMD, PAGE_SIZE);

	max_pfn_mapped = PFN_DOWN(__pa(xen_start_info->mfn_list));

	kernel_pmd = m2v(pgd[KERNEL_PGD_BOUNDARY].pgd);
	memcpy(level2_kernel_pgt, kernel_pmd, sizeof(pmd_t) * PTRS_PER_PMD);

	xen_map_identity_early(level2_kernel_pgt, max_pfn);

	memcpy(swapper_pg_dir, pgd, sizeof(pgd_t) * PTRS_PER_PGD);

	/*
	 * When running a 32 bit domain 0 on a 64 bit hypervisor a
	 * pinned L3 (such as the initial pgd here) contains bits
	 * which are reserved in the PAE layout but not in the 64 bit
	 * layout. Unfortunately some versions of the hypervisor
	 * (incorrectly) validate compat mode guests against the PAE
	 * layout and hence will not allow such a pagetable to be
	 * pinned by the guest. Therefore we mask off only the PFN and
	 * Present bits of the supplied L3.
	 */
	for (i = 0; i < PTRS_PER_PGD; i++)
		swapper_pg_dir[i].pgd &= (PTE_PFN_MASK | _PAGE_PRESENT);

	set_pgd(&swapper_pg_dir[KERNEL_PGD_BOUNDARY],
			__pgd(__pa(level2_kernel_pgt) | _PAGE_PRESENT));

	set_page_prot(level2_kernel_pgt, PAGE_KERNEL_RO);
	set_page_prot(swapper_pg_dir, PAGE_KERNEL_RO);
	set_page_prot(empty_zero_page, PAGE_KERNEL_RO);

	pin_pagetable_pfn(MMUEXT_UNPIN_TABLE, PFN_DOWN(__pa(pgd)));

	xen_write_cr3(__pa(swapper_pg_dir));

	pin_pagetable_pfn(MMUEXT_PIN_L3_TABLE, PFN_DOWN(__pa(swapper_pg_dir)));

	reserve_early(__pa(xen_start_info->pt_base),
		      __pa(xen_start_info->pt_base +
			   xen_start_info->nr_pt_frames * PAGE_SIZE),
		      "XEN PAGETABLES");

	return swapper_pg_dir;
}
#endif	/* CONFIG_X86_64 */

static unsigned char dummy_ioapic_mapping[PAGE_SIZE] __page_aligned_bss;

static void xen_set_fixmap(unsigned idx, phys_addr_t phys, pgprot_t prot)
{
	pte_t pte;

	phys >>= PAGE_SHIFT;

	switch (idx) {
	case FIX_BTMAP_END ... FIX_BTMAP_BEGIN:
#ifdef CONFIG_X86_F00F_BUG
	case FIX_F00F_IDT:
#endif
#ifdef CONFIG_X86_32
	case FIX_WP_TEST:
	case FIX_VDSO:
# ifdef CONFIG_HIGHMEM
	case FIX_KMAP_BEGIN ... FIX_KMAP_END:
# endif
#else
	case VSYSCALL_LAST_PAGE ... VSYSCALL_FIRST_PAGE:
#endif
#ifdef CONFIG_X86_LOCAL_APIC
	case FIX_APIC_BASE:	/* maps dummy local APIC */
#endif
	case FIX_TEXT_POKE0:
	case FIX_TEXT_POKE1:
		/* All local page mappings */
		pte = pfn_pte(phys, prot);
		break;

#ifdef CONFIG_X86_IO_APIC
	case FIX_IO_APIC_BASE_0 ... FIX_IO_APIC_BASE_END:
		/*
		 * We just don't map the IO APIC - all access is via
		 * hypercalls.  Keep the address in the pte for reference.
		 */
		pte = pfn_pte(PFN_DOWN(__pa(dummy_ioapic_mapping)), PAGE_KERNEL);
		break;
#endif

	case FIX_PARAVIRT_BOOTMAP:
		/* This is an MFN, but it isn't an IO mapping from the
		   IO domain */
		pte = mfn_pte(phys, prot);
		break;

	default:
		/* By default, set_fixmap is used for hardware mappings */
		pte = mfn_pte(phys, __pgprot(pgprot_val(prot) | _PAGE_IOMAP));
		break;
	}

	__native_set_fixmap(idx, pte);

#ifdef CONFIG_X86_64
	/* Replicate changes to map the vsyscall page into the user
	   pagetable vsyscall mapping. */
	if (idx >= VSYSCALL_LAST_PAGE && idx <= VSYSCALL_FIRST_PAGE) {
		unsigned long vaddr = __fix_to_virt(idx);
		set_pte_vaddr_pud(level3_user_vsyscall, vaddr, pte);
	}
#endif
}

__init void xen_ident_map_ISA(void)
{
	unsigned long pa;

	/*
	 * If we're dom0, then linear map the ISA machine addresses into
	 * the kernel's address space.
	 */
	if (!xen_initial_domain())
		return;

	xen_raw_printk("Xen: setup ISA identity maps\n");

	for (pa = ISA_START_ADDRESS; pa < ISA_END_ADDRESS; pa += PAGE_SIZE) {
		pte_t pte = mfn_pte(PFN_DOWN(pa), PAGE_KERNEL_IO);

		if (HYPERVISOR_update_va_mapping(PAGE_OFFSET + pa, pte, 0))
			BUG();
	}

	xen_flush_tlb();
}

static __init void xen_post_allocator_init(void)
{
	pv_mmu_ops.set_pte = xen_set_pte;
	pv_mmu_ops.set_pmd = xen_set_pmd;
	pv_mmu_ops.set_pud = xen_set_pud;
#if PAGETABLE_LEVELS == 4
	pv_mmu_ops.set_pgd = xen_set_pgd;
#endif

	/* This will work as long as patching hasn't happened yet
	   (which it hasn't) */
	pv_mmu_ops.alloc_pte = xen_alloc_pte;
	pv_mmu_ops.alloc_pmd = xen_alloc_pmd;
	pv_mmu_ops.release_pte = xen_release_pte;
	pv_mmu_ops.release_pmd = xen_release_pmd;
#if PAGETABLE_LEVELS == 4
	pv_mmu_ops.alloc_pud = xen_alloc_pud;
	pv_mmu_ops.release_pud = xen_release_pud;
#endif

#ifdef CONFIG_X86_64
	SetPagePinned(virt_to_page(level3_user_vsyscall));
#endif
	xen_mark_init_mm_pinned();
}

static void xen_leave_lazy_mmu(void)
{
	preempt_disable();
	xen_mc_flush();
	paravirt_leave_lazy_mmu();
	preempt_enable();
}

static const struct pv_mmu_ops xen_mmu_ops __initdata = {
	.read_cr2 = xen_read_cr2,
	.write_cr2 = xen_write_cr2,

	.read_cr3 = xen_read_cr3,
	.write_cr3 = xen_write_cr3,

	.flush_tlb_user = xen_flush_tlb,
	.flush_tlb_kernel = xen_flush_tlb,
	.flush_tlb_single = xen_flush_tlb_single,
	.flush_tlb_others = xen_flush_tlb_others,

	.pte_update = paravirt_nop,
	.pte_update_defer = paravirt_nop,

	.pgd_alloc = xen_pgd_alloc,
	.pgd_free = xen_pgd_free,

	.alloc_pte = xen_alloc_pte_init,
	.release_pte = xen_release_pte_init,
	.alloc_pmd = xen_alloc_pmd_init,
	.alloc_pmd_clone = paravirt_nop,
	.release_pmd = xen_release_pmd_init,

#ifdef CONFIG_HIGHPTE
	.kmap_atomic_pte = xen_kmap_atomic_pte,
#endif

	.set_pte = xen_set_pte_init,
	.set_pte_at = xen_set_pte_at,
	.set_pmd = xen_set_pmd_hyper,

	.ptep_modify_prot_start = __ptep_modify_prot_start,
	.ptep_modify_prot_commit = __ptep_modify_prot_commit,

	.pte_val = PV_CALLEE_SAVE(xen_pte_val),
	.pgd_val = PV_CALLEE_SAVE(xen_pgd_val),

	.make_pte = PV_CALLEE_SAVE(xen_make_pte),
	.make_pgd = PV_CALLEE_SAVE(xen_make_pgd),

#ifdef CONFIG_X86_PAE
	.set_pte_atomic = xen_set_pte_atomic,
	.pte_clear = xen_pte_clear,
	.pmd_clear = xen_pmd_clear,
#endif	/* CONFIG_X86_PAE */
	.set_pud = xen_set_pud_hyper,

	.make_pmd = PV_CALLEE_SAVE(xen_make_pmd),
	.pmd_val = PV_CALLEE_SAVE(xen_pmd_val),

#if PAGETABLE_LEVELS == 4
	.pud_val = PV_CALLEE_SAVE(xen_pud_val),
	.make_pud = PV_CALLEE_SAVE(xen_make_pud),
	.set_pgd = xen_set_pgd_hyper,

	.alloc_pud = xen_alloc_pmd_init,
	.release_pud = xen_release_pmd_init,
#endif	/* PAGETABLE_LEVELS == 4 */

	.activate_mm = xen_activate_mm,
	.dup_mmap = xen_dup_mmap,
	.exit_mmap = xen_exit_mmap,

	.lazy_mode = {
		.enter = paravirt_enter_lazy_mmu,
		.leave = xen_leave_lazy_mmu,
	},

	.set_fixmap = xen_set_fixmap,
};

void __init xen_init_mmu_ops(void)
{
	x86_init.paging.pagetable_setup_start = xen_pagetable_setup_start;
	x86_init.paging.pagetable_setup_done = xen_pagetable_setup_done;
	pv_mmu_ops = xen_mmu_ops;
}

/* Protected by xen_reservation_lock. */
#define MAX_CONTIG_ORDER 9 /* 2MB */
static unsigned long discontig_frames[1<<MAX_CONTIG_ORDER];

#define VOID_PTE (mfn_pte(0, __pgprot(0)))
static void xen_zap_pfn_range(unsigned long vaddr, unsigned int order,
				unsigned long *in_frames,
				unsigned long *out_frames)
{
	int i;
	struct multicall_space mcs;

	xen_mc_batch();
	for (i = 0; i < (1UL<<order); i++, vaddr += PAGE_SIZE) {
		mcs = __xen_mc_entry(0);

		if (in_frames)
			in_frames[i] = virt_to_mfn(vaddr);

		MULTI_update_va_mapping(mcs.mc, vaddr, VOID_PTE, 0);
		set_phys_to_machine(virt_to_pfn(vaddr), INVALID_P2M_ENTRY);

		if (out_frames)
			out_frames[i] = virt_to_pfn(vaddr);
	}
	xen_mc_issue(0);
}

/*
 * Update the pfn-to-mfn mappings for a virtual address range, either to
 * point to an array of mfns, or contiguously from a single starting
 * mfn.
 */
static void xen_remap_exchanged_ptes(unsigned long vaddr, int order,
				     unsigned long *mfns,
				     unsigned long first_mfn)
{
	unsigned i, limit;
	unsigned long mfn;

	xen_mc_batch();

	limit = 1u << order;
	for (i = 0; i < limit; i++, vaddr += PAGE_SIZE) {
		struct multicall_space mcs;
		unsigned flags;

		mcs = __xen_mc_entry(0);
		if (mfns)
			mfn = mfns[i];
		else
			mfn = first_mfn + i;

		if (i < (limit - 1))
			flags = 0;
		else {
			if (order == 0)
				flags = UVMF_INVLPG | UVMF_ALL;
			else
				flags = UVMF_TLB_FLUSH | UVMF_ALL;
		}

		MULTI_update_va_mapping(mcs.mc, vaddr,
				mfn_pte(mfn, PAGE_KERNEL), flags);

		set_phys_to_machine(virt_to_pfn(vaddr), mfn);
	}

	xen_mc_issue(0);
}

/*
 * Perform the hypercall to exchange a region of our pfns to point to
 * memory with the required contiguous alignment.  Takes the pfns as
 * input, and populates mfns as output.
 *
 * Returns a success code indicating whether the hypervisor was able to
 * satisfy the request or not.
 */
static int xen_exchange_memory(unsigned long extents_in, unsigned int order_in,
			       unsigned long *pfns_in,
			       unsigned long extents_out, unsigned int order_out,
			       unsigned long *mfns_out,
			       unsigned int address_bits)
{
	long rc;
	int success;

	struct xen_memory_exchange exchange = {
		.in = {
			.nr_extents   = extents_in,
			.extent_order = order_in,
			.extent_start = pfns_in,
			.domid        = DOMID_SELF
		},
		.out = {
			.nr_extents   = extents_out,
			.extent_order = order_out,
			.extent_start = mfns_out,
			.address_bits = address_bits,
			.domid        = DOMID_SELF
		}
	};

	BUG_ON(extents_in << order_in != extents_out << order_out);

	rc = HYPERVISOR_memory_op(XENMEM_exchange, &exchange);
	success = (exchange.nr_exchanged == extents_in);

	BUG_ON(!success && ((exchange.nr_exchanged != 0) || (rc == 0)));
	BUG_ON(success && (rc != 0));

	return success;
}

int xen_create_contiguous_region(unsigned long vstart, unsigned int order,
				 unsigned int address_bits)
{
	unsigned long *in_frames = discontig_frames, out_frame;
	unsigned long  flags;
	int            success;

	/*
	 * Currently an auto-translated guest will not perform I/O, nor will
	 * it require PAE page directories below 4GB. Therefore any calls to
	 * this function are redundant and can be ignored.
	 */

	if (xen_feature(XENFEAT_auto_translated_physmap))
		return 0;

	if (unlikely(order > MAX_CONTIG_ORDER))
		return -ENOMEM;

	memset((void *) vstart, 0, PAGE_SIZE << order);

	spin_lock_irqsave(&xen_reservation_lock, flags);

	/* 1. Zap current PTEs, remembering MFNs. */
	xen_zap_pfn_range(vstart, order, in_frames, NULL);

	/* 2. Get a new contiguous memory extent. */
	out_frame = virt_to_pfn(vstart);
	success = xen_exchange_memory(1UL << order, 0, in_frames,
				      1, order, &out_frame,
				      address_bits);

	/* 3. Map the new extent in place of old pages. */
	if (success)
		xen_remap_exchanged_ptes(vstart, order, NULL, out_frame);
	else
		xen_remap_exchanged_ptes(vstart, order, in_frames, 0);

	spin_unlock_irqrestore(&xen_reservation_lock, flags);

	return success ? 0 : -ENOMEM;
}
EXPORT_SYMBOL_GPL(xen_create_contiguous_region);

void xen_destroy_contiguous_region(unsigned long vstart, unsigned int order)
{
	unsigned long *out_frames = discontig_frames, in_frame;
	unsigned long  flags;
	int success;

	if (xen_feature(XENFEAT_auto_translated_physmap))
		return;

	if (unlikely(order > MAX_CONTIG_ORDER))
		return;

	memset((void *) vstart, 0, PAGE_SIZE << order);

	spin_lock_irqsave(&xen_reservation_lock, flags);

	/* 1. Find start MFN of contiguous extent. */
	in_frame = virt_to_mfn(vstart);

	/* 2. Zap current PTEs. */
	xen_zap_pfn_range(vstart, order, NULL, out_frames);

	/* 3. Do the exchange for non-contiguous MFNs. */
	success = xen_exchange_memory(1, order, &in_frame, 1UL << order,
					0, out_frames, 0);

	/* 4. Map new pages in place of old pages. */
	if (success)
		xen_remap_exchanged_ptes(vstart, order, out_frames, 0);
	else
		xen_remap_exchanged_ptes(vstart, order, NULL, in_frame);

	spin_unlock_irqrestore(&xen_reservation_lock, flags);
}
EXPORT_SYMBOL_GPL(xen_destroy_contiguous_region);

#define REMAP_BATCH_SIZE 16

struct remap_data {
	unsigned long mfn;
	pgprot_t prot;
	struct mmu_update *mmu_update;
};

static int remap_area_mfn_pte_fn(pte_t *ptep, pgtable_t token,
				 unsigned long addr, void *data)
{
	struct remap_data *rmd = data;
	pte_t pte = pte_mkspecial(pfn_pte(rmd->mfn, rmd->prot));

	rmd->mmu_update->ptr = arbitrary_virt_to_machine(ptep).maddr;
	rmd->mmu_update->val = pte_val_ma(pte);

	rmd->mfn++;
	rmd->mmu_update++;

	return 0;
}

static int __xen_remap_domain_mfn_range(struct mm_struct *mm,
				unsigned long addr,
				unsigned long mfn, int nr,
				pgprot_t prot, unsigned domid)
{
	struct remap_data rmd;
	struct mmu_update mmu_update[REMAP_BATCH_SIZE];
	int batch;
	unsigned long range;
	int err;

	prot = __pgprot(pgprot_val(prot) | _PAGE_IOMAP);

	rmd.mfn = mfn;
	rmd.prot = prot;

	while (nr) {
		batch = min(REMAP_BATCH_SIZE, nr);
		range = (unsigned long)batch << PAGE_SHIFT;

		rmd.mmu_update = mmu_update;

		err = apply_to_page_range(mm, addr, range,
					  remap_area_mfn_pte_fn, &rmd);
		if (err)
			goto out;

		if (HYPERVISOR_mmu_update(mmu_update, batch, NULL, domid) < 0) {
			err = -EFAULT;
			goto out;
		}

		nr -= batch;
		addr += range;
	}

	err = 0;
out:

	flush_tlb_all();

	return err;
}
int xen_remap_domain_mfn_range(struct vm_area_struct *vma,
			       unsigned long addr,
			       unsigned long mfn, int nr,
			       pgprot_t prot, unsigned domid)
{

	vma->vm_flags |= VM_IO | VM_RESERVED | VM_PFNMAP;

	return __xen_remap_domain_mfn_range(vma->vm_mm, addr, 
					mfn, nr, prot, domid);
}
EXPORT_SYMBOL_GPL(xen_remap_domain_mfn_range);

int xen_remap_domain_kernel_mfn_range(unsigned long addr,
			       unsigned long mfn, int nr,
			       pgprot_t prot, unsigned domid)
{
	return __xen_remap_domain_mfn_range(&init_mm, addr, 
					mfn, nr, prot, domid);
}
EXPORT_SYMBOL_GPL(xen_remap_domain_kernel_mfn_range);

#ifdef CONFIG_XEN_PVHVM
static void xen_hvm_exit_mmap(struct mm_struct *mm)
{
	struct xen_hvm_pagetable_dying a;
	int rc;

	a.domid = DOMID_SELF;
	a.gpa = __pa(mm->pgd);
	rc = HYPERVISOR_hvm_op(HVMOP_pagetable_dying, &a);
	WARN_ON_ONCE(rc < 0);
}

static int is_pagetable_dying_supported(void)
{
	struct xen_hvm_pagetable_dying a;
	int rc = 0;

	a.domid = DOMID_SELF;
	a.gpa = 0x00;
	rc = HYPERVISOR_hvm_op(HVMOP_pagetable_dying, &a);
	if (rc < 0) {
		printk(KERN_DEBUG "HVMOP_pagetable_dying not supported\n");
		return 0;
	}
	return 1;
}

void __init xen_hvm_init_mmu_ops(void)
{
	if (is_pagetable_dying_supported())
		pv_mmu_ops.exit_mmap = xen_hvm_exit_mmap;
}
#endif

#ifdef CONFIG_XEN_DEBUG_FS

static struct dentry *d_mmu_debug;

static int __init xen_mmu_debugfs(void)
{
	struct dentry *d_xen = xen_init_debugfs();

	if (d_xen == NULL)
		return -ENOMEM;

	d_mmu_debug = debugfs_create_dir("mmu", d_xen);

	debugfs_create_u8("zero_stats", 0644, d_mmu_debug, &zero_stats);

	debugfs_create_u32("pgd_update", 0444, d_mmu_debug, &mmu_stats.pgd_update);
	debugfs_create_u32("pgd_update_pinned", 0444, d_mmu_debug,
			   &mmu_stats.pgd_update_pinned);
	debugfs_create_u32("pgd_update_batched", 0444, d_mmu_debug,
			   &mmu_stats.pgd_update_pinned);

	debugfs_create_u32("pud_update", 0444, d_mmu_debug, &mmu_stats.pud_update);
	debugfs_create_u32("pud_update_pinned", 0444, d_mmu_debug,
			   &mmu_stats.pud_update_pinned);
	debugfs_create_u32("pud_update_batched", 0444, d_mmu_debug,
			   &mmu_stats.pud_update_pinned);

	debugfs_create_u32("pmd_update", 0444, d_mmu_debug, &mmu_stats.pmd_update);
	debugfs_create_u32("pmd_update_pinned", 0444, d_mmu_debug,
			   &mmu_stats.pmd_update_pinned);
	debugfs_create_u32("pmd_update_batched", 0444, d_mmu_debug,
			   &mmu_stats.pmd_update_pinned);

	debugfs_create_u32("pte_update", 0444, d_mmu_debug, &mmu_stats.pte_update);
//	debugfs_create_u32("pte_update_pinned", 0444, d_mmu_debug,
//			   &mmu_stats.pte_update_pinned);
	debugfs_create_u32("pte_update_batched", 0444, d_mmu_debug,
			   &mmu_stats.pte_update_pinned);

	debugfs_create_u32("mmu_update", 0444, d_mmu_debug, &mmu_stats.mmu_update);
	debugfs_create_u32("mmu_update_extended", 0444, d_mmu_debug,
			   &mmu_stats.mmu_update_extended);
	xen_debugfs_create_u32_array("mmu_update_histo", 0444, d_mmu_debug,
				     mmu_stats.mmu_update_histo, 20);

	debugfs_create_u32("set_pte_at", 0444, d_mmu_debug, &mmu_stats.set_pte_at);
	debugfs_create_u32("set_pte_at_batched", 0444, d_mmu_debug,
			   &mmu_stats.set_pte_at_batched);
	debugfs_create_u32("set_pte_at_current", 0444, d_mmu_debug,
			   &mmu_stats.set_pte_at_current);
	debugfs_create_u32("set_pte_at_kernel", 0444, d_mmu_debug,
			   &mmu_stats.set_pte_at_kernel);

	debugfs_create_u32("prot_commit", 0444, d_mmu_debug, &mmu_stats.prot_commit);
	debugfs_create_u32("prot_commit_batched", 0444, d_mmu_debug,
			   &mmu_stats.prot_commit_batched);

	return 0;
}
fs_initcall(xen_mmu_debugfs);

#endif	/* CONFIG_XEN_DEBUG_FS */
