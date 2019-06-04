/*
 * drivers/amlogic/memory_ext/vmap_stack.c
 *
 * Copyright (C) 2017 Amlogic, Inc. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 */

#include <linux/version.h>
#include <linux/types.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/mutex.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/reboot.h>
#include <linux/memblock.h>
#include <linux/vmalloc.h>
#include <linux/arm-smccc.h>
#include <linux/memcontrol.h>
#include <linux/amlogic/vmap_stack.h>
#include <linux/highmem.h>
#include <asm/tlbflush.h>
#include <asm/stacktrace.h>

#define DEBUG							0

#define D(format, args...)					\
	{ if (DEBUG)						\
		pr_info("VMAP:%s "format, __func__, ##args);	\
	}

#define E(format, args...)	pr_err("VMAP:%s "format, __func__, ##args)

static unsigned long stack_shrink_jiffies;
static unsigned char vmap_shrink_enable;
static atomic_t vmap_stack_size;
static struct aml_vmap *avmap;

DEFINE_PER_CPU(unsigned long [THREAD_SIZE/sizeof(long)], vmap_stack)
	__aligned(16);

void update_vmap_stack(int diff)
{
	atomic_add(diff, &vmap_stack_size);
}
EXPORT_SYMBOL(update_vmap_stack);

int get_vmap_stack_size(void)
{
	return atomic_read(&vmap_stack_size);
}
EXPORT_SYMBOL(get_vmap_stack_size);

static int is_vmap_addr(unsigned long addr)
{
	unsigned long start, end;

	start = (unsigned long)avmap->root_vm->addr;
	end   = (unsigned long)avmap->root_vm->addr + avmap->root_vm->size;
	if ((addr >= start) && (addr < end))
		return 1;
	else
		return 0;
}

static struct page *get_vmap_cached_page(int *remain)
{
	unsigned long flags;
	struct page *page;

	spin_lock_irqsave(&avmap->page_lock, flags);
	if (unlikely(!avmap->cached_pages)) {
		spin_unlock_irqrestore(&avmap->page_lock, flags);
		return NULL;
	}
	page = list_first_entry(&avmap->list, struct page, lru);
	list_del(&page->lru);
	avmap->cached_pages--;
	*remain = avmap->cached_pages;
	spin_unlock_irqrestore(&avmap->page_lock, flags);

	return page;
}

static int vmap_mmu_set(struct page *page, unsigned long addr, int set)
{
	pgd_t *pgd = NULL;
	pud_t *pud = NULL;
	pmd_t *pmd = NULL;
	pte_t *pte = NULL;

	pgd = pgd_offset_k(addr);
	pud = pud_alloc(&init_mm, pgd, addr);
	if (!pud)
		goto nomem;

	if (pud_none(*pud)) {
		pmd = pmd_alloc(&init_mm, pud, addr);
		if (!pmd)
			goto nomem;
	}

	pmd = pmd_offset(pud, addr);
	if (pmd_none(*pmd)) {
		pte = pte_alloc_kernel(pmd, addr);
		if (!pte)
			goto nomem;
	}

	pte = pte_offset_map(pmd, addr);
	if (set)
		set_pte_at(&init_mm, addr, pte, mk_pte(page, PAGE_KERNEL));
	else
		pte_clear(&init_mm, addr, pte);
	pte_unmap(pte);
	flush_tlb_kernel_range(addr, addr + PAGE_SIZE);
	D("add:%lx, pgd:%p %llx, pmd:%p %llx, pte:%p %llx\n",
		addr, pgd, pgd_val(*pgd), pmd, pmd_val(*pmd),
		pte, pte_val(*pte));
	return 0;
nomem:
	E("allocation page talbe failed, G:%p, U:%p, M:%p, T:%p",
		pgd, pud, pmd, pte);
	return -ENOMEM;
}

static int stack_floor_page(unsigned long addr)
{
	/*
	 * stack address must align to THREAD_SIZE
	 */
	return ((addr & (THREAD_SIZE - 1)) < PAGE_SIZE);
}

static int check_addr_up_flow(unsigned long addr)
{
	/*
	 * It's the first page of 4 contigours virtual address
	 * rage(aligned to THREAD_SIZE) but next page of this
	 * addr is not mapped
	 */
	if (stack_floor_page(addr) &&
	    !vmalloc_to_page((const void *)(addr + PAGE_SIZE)))
		return 1;
	return 0;
}

#if DEBUG
static void dump_backtrace_entry(unsigned long ip, unsigned long fp)
{
	unsigned long fp_size = 0;

	if (fp >= VMALLOC_START) {
		fp_size = *((unsigned long *)fp) - fp;
		/* fp cross IRQ or vmap stack */
		if (fp_size >= THREAD_SIZE)
			fp_size = 0;
	}
	pr_info("[%016lx+%4ld][<%p>] %pS\n",
		fp, fp_size, (void *) ip, (void *) ip);
}

static void show_fault_stack(unsigned long addr, struct pt_regs *regs)
{
	struct stackframe frame;

	frame.fp = regs->regs[29];
	frame.sp = addr;
	frame.pc = (unsigned long)regs->regs[30];

	pr_info("Call trace:\n");
	pr_info("[%016lx+%4ld][<%p>] %pS\n",
		addr, frame.fp - addr, (void *)regs->pc, (void *) regs->pc);
	while (1) {
		int ret;

		dump_backtrace_entry(frame.pc, frame.fp);
		ret = unwind_frame(current, &frame);
		if (ret < 0)
			break;
	}
}
#endif

/*
 * IRQ should *NEVER* been opened in this handler
 */
int handle_vmap_fault(unsigned long addr, unsigned int esr,
		      struct pt_regs *regs)
{
	struct page *page;
	int cache = 0;

	if (!is_vmap_addr(addr))
		return -EINVAL;

	D("addr:%lx, esr:%x, task:%5d %s\n",
		addr, esr, current->pid, current->comm);
	D("pc:%pf, %llx, lr:%pf, %llx, sp:%llx, %lx\n",
		(void *)regs->pc, regs->pc,
		(void *)regs->regs[30], regs->regs[30], regs->sp,
		current_stack_pointer);

	if (check_addr_up_flow(addr)) {
		E("address %lx out of range\n", addr);
		E("PC is:%llx, %pf, LR is:%llx %pf\n",
			regs->pc, (void *)regs->pc,
			regs->regs[30], (void *)regs->regs[30]);
		E("task:%d %s, stack:%p, %lx\n",
			current->pid, current->comm, current->stack,
			current_stack_pointer);
		dump_stack();
		return -ERANGE;
	}

	/*
	 * allocate a new page for vmap
	 */
	page = get_vmap_cached_page(&cache);
	WARN_ON(!page);
	vmap_mmu_set(page, addr, 1);
	update_vmap_stack(1);
	if ((THREAD_SIZE_ORDER  > 1) && stack_floor_page(addr)) {
		E("task:%d %s, stack near overflow, addr:%lx\n",
			current->pid, current->comm, addr);
		dump_stack();
	}

	/* cache is not enough */
	if (cache <= (VMAP_CACHE_PAGE / 2))
		mod_delayed_work(system_highpri_wq, &avmap->mwork, 0);

	D("map page:%5lx for addr:%lx\n", page_to_pfn(page), addr);
#if DEBUG
	show_fault_stack(addr, regs);
#endif

	return 0;
}
EXPORT_SYMBOL(handle_vmap_fault);

static unsigned long vmap_shrink_count(struct shrinker *s,
				  struct shrink_control *sc)
{
	return global_page_state(NR_KERNEL_STACK_KB);
}

static int shrink_vm_stack(unsigned long low, unsigned long high)
{
	int pages = 0;
	struct page *page;

	for (; low < (high & PAGE_MASK); low += PAGE_SIZE) {
		page = vmalloc_to_page((const void *)low);
		vmap_mmu_set(page, low, 0);
		update_vmap_stack(-1);
		__free_page(page);
		pages++;
	}
	return pages;
}

static unsigned long get_task_stack_floor(unsigned long sp)
{
	unsigned long end;

	end = sp & (THREAD_SIZE - 1);
	while (sp > end) {
		if (!vmalloc_to_page((const void *)sp))
			break;
		sp -= PAGE_SIZE;
	}
	return PAGE_ALIGN(sp);
}

static unsigned long vmap_shrink_scan(struct shrinker *s,
				      struct shrink_control *sc)
{
	struct task_struct *tsk;
	unsigned long thread_sp;
	unsigned long stack_floor;
	unsigned long rem = 0;

	if (!vmap_shrink_enable)
		return 0;

	/*
	 * sleep for a while if shrink too ofen
	 */
	if (jiffies - stack_shrink_jiffies <= STACK_SHRINK_SLEEP)
		return 0;

	rcu_read_lock();
	for_each_process(tsk) {
		thread_sp = thread_saved_sp(tsk);
		stack_floor = get_task_stack_floor(thread_sp);
		/*
		 * Make sure selected task is sleeping
		 */
		D("r:%3ld, sp:[%lx-%lx], s:%5ld, tsk:%lx %d %s\n",
			rem, thread_sp, stack_floor,
			thread_sp - stack_floor,
			tsk->state, tsk->pid, tsk->comm);
		task_lock(tsk);
		if (tsk->state == TASK_RUNNING) {
			task_unlock(tsk);
			continue;
		}
		if (thread_sp - stack_floor >= STACK_SHRINK_THRESHOLD)
			rem += shrink_vm_stack(stack_floor, thread_sp);
		task_unlock(tsk);
	}
	rcu_read_unlock();
	stack_shrink_jiffies = jiffies;

	return rem;
}

static struct shrinker vmap_shrinker = {
	.scan_objects = vmap_shrink_scan,
	.count_objects = vmap_shrink_count,
	.seeks = DEFAULT_SEEKS * 16
};

/* FOR debug */
static unsigned long vmap_debug_jiff;

void aml_account_task_stack(struct task_struct *tsk, int account)
{
	unsigned long stack = (unsigned long)task_stack_page(tsk);
	struct page *first_page;

	stack += STACK_TOP_PAGE_OFF;
	first_page = vmalloc_to_page((void *)stack);
	mod_zone_page_state(page_zone(first_page), NR_KERNEL_STACK_KB,
			    THREAD_SIZE / 1024 * account);

	memcg_kmem_update_page_stat(first_page, MEMCG_KERNEL_STACK_KB,
				    account * (THREAD_SIZE / 1024));
	if (time_after(jiffies, vmap_debug_jiff + HZ * 5)) {
		int ratio, rem;

		vmap_debug_jiff = jiffies;
		ratio = ((get_vmap_stack_size() << (PAGE_SHIFT - 10)) * 10000) /
			global_page_state(NR_KERNEL_STACK_KB);
		rem   = ratio % 100;
		D("STACK:%ld KB, vmap:%d KB, cached:%d KB, rate:%2d.%02d%%\n",
			global_page_state(NR_KERNEL_STACK_KB),
			get_vmap_stack_size() << (PAGE_SHIFT - 10),
			avmap->cached_pages << (PAGE_SHIFT - 10),
			ratio / 100, rem);
	}
}

void *aml_stack_alloc(int node, struct task_struct *tsk)
{
	unsigned long bitmap_no, raw_start;
	struct page *page;
	unsigned long addr, map_addr, flags;

	spin_lock_irqsave(&avmap->vmap_lock, flags);
	raw_start = avmap->start_bit;
	bitmap_no = find_next_zero_bit(avmap->bitmap, MAX_TASKS,
				       avmap->start_bit);
	avmap->start_bit = bitmap_no + 1; /* next idle address space */
	if (bitmap_no >= MAX_TASKS) {
		spin_unlock_irqrestore(&avmap->vmap_lock, flags);
		E("BITMAP FULL!!!\n");
		return NULL;
	}
	bitmap_set(avmap->bitmap, bitmap_no, 1);
	spin_unlock_irqrestore(&avmap->vmap_lock, flags);

	page = alloc_page(THREADINFO_GFP | __GFP_ZERO);
	if (!page) {
		spin_lock_irqsave(&avmap->vmap_lock, flags);
		bitmap_clear(avmap->bitmap, bitmap_no, 1);
		spin_unlock_irqrestore(&avmap->vmap_lock, flags);
		E("alloction page failed\n");
		return NULL;
	}
	/*
	 * map first page only
	 */
	addr = (unsigned long)avmap->root_vm->addr + THREAD_SIZE * bitmap_no;
	map_addr = addr + STACK_TOP_PAGE_OFF;
	vmap_mmu_set(page, map_addr, 1);
	update_vmap_stack(1);
	D("bit idx:%5ld, start:%5ld, addr:%lx, page:%lx\n",
		bitmap_no, raw_start, addr, page_to_pfn(page));

	return (void *)addr;
}

void aml_stack_free(struct task_struct *tsk)
{
	unsigned long stack = (unsigned long)tsk->stack;
	unsigned long addr, bitmap_no;
	struct page *page;
	unsigned long flags;

	addr = stack + STACK_TOP_PAGE_OFF;
	for (; addr >= stack; addr -= PAGE_SIZE) {
		page = vmalloc_to_page((const void *)addr);
		if (!page)
			break;
		vmap_mmu_set(page, addr, 0);
		/* supplement for stack page cache first */
		spin_lock_irqsave(&avmap->page_lock, flags);
		if (avmap->cached_pages < VMAP_CACHE_PAGE) {
			list_add_tail(&page->lru, &avmap->list);
			avmap->cached_pages++;
			spin_unlock_irqrestore(&avmap->page_lock, flags);
			clear_highpage(page);	/* clear for next use */
		} else {
			spin_unlock_irqrestore(&avmap->page_lock, flags);
			__free_page(page);
		}
		update_vmap_stack(-1);
	}
	bitmap_no = (stack - (unsigned long)avmap->root_vm->addr) / THREAD_SIZE;
	spin_lock_irqsave(&avmap->vmap_lock, flags);
	bitmap_clear(avmap->bitmap, bitmap_no, 1);
	if (bitmap_no < avmap->start_bit)
		avmap->start_bit = bitmap_no;
	spin_unlock_irqrestore(&avmap->vmap_lock, flags);
}

static void page_cache_maintain_work(struct work_struct *work)
{
	struct page *page;
	struct list_head head;
	int i, cnt;
	unsigned long flags;

	spin_lock_irqsave(&avmap->page_lock, flags);
	cnt = avmap->cached_pages;
	spin_unlock_irqrestore(&avmap->page_lock, flags);
	if (cnt >= VMAP_CACHE_PAGE) {
		D("cache full cnt:%d\n", cnt);
		schedule_delayed_work(&avmap->mwork, CACHE_MAINTAIN_DELAY);
		return;
	}

	INIT_LIST_HEAD(&head);
	for (i = 0; i < VMAP_CACHE_PAGE - cnt; i++) {
		page = alloc_page(GFP_KERNEL | __GFP_HIGH);
		if (!page) {
			E("get page failed, allocated:%d, cnt:%d\n", i, cnt);
			break;
		}
		list_add(&page->lru, &head);
	}
	spin_lock_irqsave(&avmap->page_lock, flags);
	list_splice(&head, &avmap->list);
	avmap->cached_pages += i;
	spin_unlock_irqrestore(&avmap->page_lock, flags);
	D("add %d pages, cnt:%d\n", i, cnt);
	schedule_delayed_work(&avmap->mwork, CACHE_MAINTAIN_DELAY);
}

int __init start_thread_work(void)
{
	schedule_delayed_work(&avmap->mwork, CACHE_MAINTAIN_DELAY);
	return 0;
}
arch_initcall(start_thread_work);

void __init thread_stack_cache_init(void)
{
	int i;
	unsigned long addr;
	struct page *page;

	page = alloc_pages(GFP_KERNEL, VMAP_CACHE_PAGE_ORDER);
	if (!page)
		return;

	avmap = kzalloc(sizeof(struct aml_vmap), GFP_KERNEL);
	if (!avmap) {
		__free_pages(page, VMAP_CACHE_PAGE_ORDER);
		return;
	}

	avmap->bitmap = kzalloc(MAX_TASKS / 8, GFP_KERNEL);
	if (!avmap->bitmap) {
		__free_pages(page, VMAP_CACHE_PAGE_ORDER);
		kfree(avmap);
		return;
	}
	pr_info("%s, vmap:%p, bitmap:%p, cache page:%lx\n",
		__func__, avmap, avmap->bitmap, page_to_pfn(page));
	avmap->root_vm = __get_vm_area_node(VM_STACK_AREA_SIZE,
					    VM_STACK_AREA_SIZE,
					    0, VMALLOC_START, VMALLOC_END,
					    NUMA_NO_NODE, GFP_KERNEL,
					    __builtin_return_address(0));
	if (!avmap->root_vm) {
		__free_pages(page, VMAP_CACHE_PAGE_ORDER);
		kfree(avmap->bitmap);
		kfree(avmap);
		return;
	}
	pr_info("%s, allocation vm area:%p, addr:%p, size:%lx\n", __func__,
		avmap->root_vm, avmap->root_vm->addr,
		avmap->root_vm->size);

	INIT_LIST_HEAD(&avmap->list);
	spin_lock_init(&avmap->page_lock);
	spin_lock_init(&avmap->vmap_lock);

	for (i = 0; i < VMAP_CACHE_PAGE; i++) {
		list_add(&page->lru, &avmap->list);
		page++;
	}
	avmap->cached_pages = VMAP_CACHE_PAGE;
	INIT_DELAYED_WORK(&avmap->mwork, page_cache_maintain_work);

	for_each_possible_cpu(i) {
		addr = (unsigned long)per_cpu_ptr(vmap_stack, i);
		pr_info("cpu %d, vmap_stack:[%lx-%lx]\n",
			i, addr, addr + THREAD_START_SP);
		addr = (unsigned long)per_cpu_ptr(irq_stack, i);
		pr_info("cpu %d, irq_stack: [%lx-%lx]\n",
			i, addr, addr + THREAD_START_SP);
	}
	register_shrinker(&vmap_shrinker);
}