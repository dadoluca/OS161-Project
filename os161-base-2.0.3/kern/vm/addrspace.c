/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <addrspace.h>
#include <vm.h>
#include <proc.h>

#include <spl.h>
#include <spinlock.h>
#include <current.h>
#include <mips/tlb.h>

/*
 * Note! If OPT_DUMBVM is set, as is the case until you start the VM
 * assignment, this file is not compiled or linked or in any way
 * used. The cheesy hack versions in dumbvm.c are used instead.
 */

struct addrspace *
as_create(void)
{
	struct addrspace *as;

	as = kmalloc(sizeof(struct addrspace));
	if (as == NULL) {
		return NULL;
	}

	as->ptable = (paddr_t **)alloc_kpages(1);
	if (as->ptable == NULL) {
	    kfree(as);
	    return NULL;
    }
	int i;
     /* Fill the new allocated page table with NULL, as there
      * is no pages allocated yet.
      */
	for (i = 0; i < PAGETABLE_SIZE; i++) {
	    as->ptable[i] = NULL;
	}
	as->regions = NULL;
	return as;
}

int
as_copy(struct addrspace *old, struct addrspace **ret)
{
	struct addrspace *newas;

	newas = as_create();
	if (newas==NULL) {
		return ENOMEM;
	}


	 /* Dirty bit of a region is set if a region is writeable. */
    int dirty;
	
    /* Copy regions from old to new address space */
    struct region *cur_old = old->regions;
    struct region *cur_new = newas->regions;
    /* Regions are implemented as a linked list, 
     * hence we traverse the list to copy each region.
     */
    while (cur_old != NULL) {
        struct region *reg = kmalloc(sizeof(struct region));
        if (reg == NULL) {
            as_destroy(newas);
            return ENOMEM;
        }
        reg->vbase = cur_old->vbase;
        reg->npages = cur_old->npages;
        reg->writeable_bit = cur_old->writeable_bit;
        reg->old_writeable_bit = cur_old->old_writeable_bit;
        reg->next = NULL;
        if (cur_new != NULL) {
            cur_new->next = reg;
        } else {
            newas->regions = reg;
        }
        cur_new = reg;
        cur_old = cur_old->next;
    }

    /* Now, deep copy the page table. */
    int i, j;
    for (i = 0; i < PAGETABLE_SIZE; i++) {
        if (old->ptable[i] != NULL) {
            newas->ptable[i] = kmalloc(sizeof(paddr_t)*PAGETABLE_SIZE);
            for (j = 0; j < PAGETABLE_SIZE; j++) {
                if (old->ptable[i][j] != 0) {    
                    vaddr_t new_frame_addr = alloc_kpages(1);
                    memmove((void *)new_frame_addr, (const void *)PADDR_TO_KVADDR(old->ptable[i][j] & PAGE_FRAME), PAGE_SIZE);
                    dirty = old->ptable[i][j] & TLBLO_DIRTY;
                    newas->ptable[i][j] = (KVADDR_TO_PADDR(new_frame_addr) & PAGE_FRAME) | dirty | TLBLO_VALID;
                } else {
                    newas->ptable[i][j] = 0;
                }
            }
        }
    }


	(void)old;

	*ret = newas;
	return 0;
}

void
as_destroy(struct addrspace *as)
{
	/*
	 * Clean up as needed.
	 */
	/* Clean up the page tables. */	 
     int i, j;
    for (i = 0; i < PAGETABLE_SIZE; i++) {
        if (as->ptable[i] != NULL) {
            for (j = 0; j < PAGETABLE_SIZE; j++) {
                if (as->ptable[i][j] != 0) {
                    free_kpages(PADDR_TO_KVADDR(as->ptable[i][j] & PAGE_FRAME));
                }
            }
            kfree(as->ptable[i]);
        } 
    }
    kfree(as->ptable);

    /* Free the list of struct regions. */
    struct region *cur, *prev;
    cur = prev = as->regions;
    while (cur != NULL) {
        cur = cur->next;
        kfree(prev);
        prev = cur;
    }
    if (prev != NULL) {
        kfree(prev);
    }

	kfree(as);
}

void
as_activate(void)
{
	struct addrspace *as;

	as = proc_getas();
	if (as == NULL) {
		/*
		 * Kernel thread without an address space; leave the
		 * prior address space in place.
		 */
		return;
	}

	/* Disable interrupts on this CPU while frobbing the TLB. */
    int spl = splhigh();
    vm_tlbflush();
    splx(spl);
}

void
as_deactivate(void)
{
    /*
     * Write this. For many designs it won't need to actually do
     * anything. See proc.c for an explanation of why it (might)
     * be needed.
     */
	
    struct addrspace *as;
    as = proc_getas();
    if (as == NULL) {
        /*
         * Kernel thread without an address space; leave the
         * prior address space in place.
         */
        return;
    }

    /* Disable interrupts on this CPU while frobbing the TLB. */
    int spl = splhigh();
    vm_tlbflush();
    splx(spl);
}

/*
 * Set up a segment at virtual address VADDR of size MEMSIZE. The
 * segment in memory extends from VADDR up to (but not including)
 * VADDR+MEMSIZE.
 *
 * The READABLE, WRITEABLE, and EXECUTABLE flags are set if read,
 * write, or execute permission should be set on the segment. At the
 * moment, these are ignored. When you write the VM system, you may
 * want to implement them.
 */
int
as_define_region(struct addrspace *as, vaddr_t vaddr, size_t memsize,
		 int readable, int writeable, int executable)
{
    size_t npages;
	
    /* Align the region. First, the base... */
    memsize += vaddr & ~(vaddr_t)PAGE_FRAME;
    vaddr &= PAGE_FRAME;

    /* ...and now the length. */
    memsize = (memsize + PAGE_SIZE - 1) & PAGE_FRAME;

    npages = memsize / PAGE_SIZE;
    
    struct region *reg = kmalloc(sizeof(struct region));
    if (reg == NULL) {
        return ENOMEM;
    }

    reg->vbase = vaddr;
    reg->npages = npages;
    reg->writeable_bit = writeable;
    reg->old_writeable_bit = reg->writeable_bit;
    reg->next = NULL;
    
    /* Add the new region to the list of regions. */
    if (as->regions == NULL) {
        as->regions = reg;
    } else {
        struct region *cur, *prev;
        cur = prev = as->regions;
        while (cur != NULL && cur->vbase < reg->vbase) {
            prev = cur;
            cur = cur->next;
        }
        prev->next = reg;
        reg->next = cur;
    }
    
    /* Current implementation only cares about whether 
     * the region is writeable or not. 
     */
    (void) readable;
    (void) executable;
    return 0; 
}

/* Prepare the regions to be loaded. This is done
 * by making all read-only regions writeable temporarily.
 * Called before loading elf segments.
 */
int
as_prepare_load(struct addrspace *as)
{
    struct region *cur = as->regions;
    while (cur != NULL) {
        cur->writeable_bit = 1;
        cur = cur->next;
    }
    return 0;
}

/* After the load is complete, returns all the read-only
 * regions to its original state. Called after loading 
 * elf segments completed.
 */
int
as_complete_load(struct addrspace *as)
{
    struct region *cur = as->regions;
    while (cur != NULL) {
        cur->writeable_bit = cur->old_writeable_bit;
        cur = cur->next;
    }

    /* After changing the write permission of read-only regions,
     * flush the TLB in case it still caches read-only regions
     * as read-and-write.
     */    

    /* Disable interrupts on this CPU while frobbing the TLB. */
    int spl = splhigh();
    vm_tlbflush();
    splx(spl);
    return 0;
}

/* Define the user stack at the top of KUSEG. 
 * Set the user stack pointer at the top of user stack. 
 */
int
as_define_stack(struct addrspace *as, vaddr_t *stackptr)
{
    int result = as_define_region(as, USERSTACK - USERSTACKSIZE, USERSTACKSIZE, 1, 1, 1);
    if (result) {
        return result;
    }
    
    /* Initial user-level stack pointer. */
    *stackptr = USERSTACK;

    return 0;
}

