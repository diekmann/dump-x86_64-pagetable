#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/init.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/debugfs.h>

#ifndef __x86_64__
  #error "wrong arch"
#endif

#ifndef X86_CR4_PKE
//defined in Linux >= 4.6
#define X86_CR4_PKE_BIT		22 /* enable Protection Keys support */
#define X86_CR4_PKE		_BITUL(X86_CR4_PKE_BIT)
#endif

#define CORNY_ASSERT(cond) do { if (!(cond)) {printk("Assertion failed: "#cond" at %s, line %d.\n", __FILE__, __LINE__);} } while (0)

static inline u64 bitmask_numbits(int numbits)
{
	return (1LL << numbits) - 1;
}

struct page_table{
	void* entry[512];
};


/* page table bits 51:M reserved, must be 0. Create global bitmask once.*/
static u64 pte_reserved_flags;


static int check_entry(u64 e){
	/* 51:M reserved, must be 0*/
	if(e & pte_reserved_flags){
		printk("invalid entry!\n");
		return 0;
	}
	if(e & _PAGE_PSE){
		printk("must be 0!\n");
		return 0;
	}
	if(!(e & _PAGE_PRESENT) && e){
		printk("strange entry!\n");
		return 1;
	}
	return 1;
}

static struct dumptbl_state {
	int maxphyaddr;

	struct page_table *pml4; /*pointer to pml4 page table in virtual memory*/
	int pml4_i; /*current index into the pml4, points to a pml4e (pml4 entry)*/
	u64 pml4_baddr; /*virtual memory base address mapped by current pml4e*/

	struct page_table *pdpt;
	int pdpt_i;
	u64 pdpt_baddr;
};

int dump_pdpt_entry(struct dumptbl_state *state)
{
	CORNY_ASSERT(state->pml4_i < 512);
	CORNY_ASSERT(state->pdpt_i < 512);

	u64 addr_max; //maximum virtual address described by the current page table entry
	u64 e = (u64)state->pdpt->entry[state->pdpt_i];

	/* The position of the bit of the virtual memory address that the current page table level refers to.
	 * PML4 39:47 (inclusive)
	 * PDPT 30:38 (inclusive)
	 * */
	int bitpos = 30;

	CORNY_ASSERT(check_entry(e));
	if(!(e & _PAGE_PRESENT)){
		/*skip page which is marked not present*/
		goto increment_out;
	}
	state->pdpt_baddr = ((u64)state->pdpt_i) << bitpos;
	CORNY_ASSERT((state->pml4_baddr & state->pdpt_baddr) == 0); //no overlapping bits
	state->pdpt_baddr |= state->pml4_baddr;
	addr_max = bitmask_numbits(bitpos);
	CORNY_ASSERT((state->pml4_baddr & addr_max) == 0); //no overlapping bits
	CORNY_ASSERT((state->pdpt_baddr & addr_max) == 0); //no overlapping bits
	addr_max |= state->pdpt_baddr;
	printk("  v %p %p %s %s %s %s %s %s\n",
		(void*)state->pdpt_baddr, (void*)addr_max,
		e & _PAGE_RW ? "W" : "R",
		e & _PAGE_USER ? "U" : "K" /*kernel*/,
		e & _PAGE_PWT ? "PWT" : "",
		e & _PAGE_PCD ? "PCD" : "",
		e & _PAGE_ACCESSED ? "A" : "",
		e & _PAGE_NX ? "NX" : ""
		);
	if(e & _PAGE_PSE){
		printk("1GB page\n");
		goto increment_out;
	}

increment_out:
	if(++state->pdpt_i >= 512){
		state->pdpt_i = 0;
		return 0;
	}else{
		return 1;
	}
}


static int dump_pagetable(void)
{

	struct dumptbl_state state = {0};

	/* https://software.intel.com/sites/default/files/managed/39/c5/325462-sdm-vol-1-2abcd-3abcd.pdf pp.2783
	 * March 2017, version 062 */

	/*Chap 3.2.1 64-Bit Mode Execution Environment. Control registers expand to 64 bits*/
	u64 cr0, cr3, cr4 = 0;
	u64 ia32_efer = 0;

	BUILD_BUG_ON(sizeof(struct page_table) != 4096);
	BUILD_BUG_ON(sizeof(void *) != sizeof(u64));
	BUILD_BUG_ON(sizeof(struct page_table) / sizeof(void *) != 512);
	BUILD_BUG_ON(PAGE_SIZE != 4096);
	BUILD_BUG_ON(sizeof(pteval_t) != sizeof(u64));

	printk("(%s) x86_phys_bits %hu\n", boot_cpu_data.x86_vendor_id, boot_cpu_data.x86_phys_bits);

	if(boot_cpu_data.x86_phys_bits != 36){
		printk("MAXPHYADDR should usually be 36 but is is %u\n", boot_cpu_data.x86_phys_bits);
	}
	state.maxphyaddr = boot_cpu_data.x86_phys_bits;

	pte_reserved_flags = bitmask_numbits(51 - state.maxphyaddr + 1); //bitmap with up to 51 bit set
	pte_reserved_flags <<= state.maxphyaddr;

	__asm__ __volatile__ ("mov %%cr0, %%rax \n mov %%rax,%0": "=m" (cr0) : /*InputOperands*/ : "rax");
	__asm__ __volatile__ ("mov %%cr3, %%rax \n mov %%rax,%0": "=m" (cr3) : /*InputOperands*/ : "rax");
	__asm__ __volatile__ ("mov %%cr4, %%rax \n mov %%rax,%0": "=m" (cr4) : /*InputOperands*/ : "rax");

	if(rdmsrl_safe(MSR_EFER, &ia32_efer)){
		printk("Error reading MSR\n");
		return 1;
	}

	if(!(ia32_efer & EFER_NX)){
		printk("No IA32_EFER.NXE?????\n");
	}


	printk("cr3: 0x%llx\n", cr3);
	if(cr3 & 0xfe7 /*ignored*/
	   || (cr3 & (0xffffffffffffffffLL << state.maxphyaddr) /*reserved*/)){
		printk("cr3 looks shady\n");
	}
	if(cr3 & X86_CR3_PWT || cr3 & X86_CR3_PCD){
		printk("unexpected options in cr3\n");
	}
	if(cr0 & X86_CR0_PG  &&  cr4 & X86_CR4_PAE  &&  ia32_efer & EFER_LME  &&  !(cr4 & X86_CR4_PCIDE)){
		printk("paging according to Tab 4-12 p. 2783 intel dev manual (March 2017)\n");
	}else{
		printk("unknown paging setup\n");
		return -EPERM; /*I'm afraid I can't do that*/
	}
	if(!(cr4 & X86_CR4_PKE)){
		printk("No protection keys enabled (this is normal)\n");
	}

	state.pml4 = phys_to_virt(cr3);
	printk("page table in virtual memory at %p\n", state.pml4);
	if(!virt_addr_valid(state.pml4) || !IS_ALIGNED(cr3, 4096)){
		printk("invalid addr!\n");
		return 1; /*error*/
	}

	phys_addr_t pdpt_addr;

	u64 bm; //bitmap
	u64 addr_max;
	//walk the outermost page table
	for(state.pml4_i = 0; state.pml4_i < 512; ++state.pml4_i){
		u64 e = (u64)state.pml4->entry[state.pml4_i];
		CORNY_ASSERT(check_entry(e));
		if(!(e & _PAGE_PRESENT)){
			/*skip page which is marked not present*/
			continue;
		}
		printk("entry %p\n", (void*)e);
		state.pml4_baddr = state.pml4_i;
		state.pml4_baddr <<= 39; //bits 39:47 (inclusive)
		addr_max = 0x7fffffffffLL; //2**39-1
		if(state.pml4_baddr & (1LL << 47) /*highest bit set*/){
			CORNY_ASSERT((state.pml4_baddr & 0xffffLL<<48) == 0);
			CORNY_ASSERT((addr_max & 0xffffLL<<48) == 0);
			state.pml4_baddr |= (0xffffLL << 48);
		}
		addr_max |= state.pml4_baddr;
		printk("v %p %p %s %s %s %s %s %s\n",
			(void*)state.pml4_baddr, (void*)addr_max,
			e & _PAGE_RW ? "W" : "R",
			e & _PAGE_USER ? "U" : "K" /*kernel*/,
			e & _PAGE_PWT ? "PWT" : "",
			e & _PAGE_PCD ? "PCD" : "",
			e & _PAGE_ACCESSED ? "A" : "",
			e & _PAGE_NX ? "NX" : ""
			);

		/*phsical address of next page table level*/
		bm = bitmask_numbits(51 - 12 + 1);
		bm <<= 12;
		pdpt_addr = e & bm;

		state.pdpt = phys_to_virt(pdpt_addr);
		if(!virt_addr_valid(state.pdpt) || !IS_ALIGNED(pdpt_addr, 4096)){
			printk("pdpt invalid addr!\n");
			return 1; /*error*/
		}
		while(dump_pdpt_entry(&state)){};
	}
	return 0;
}

static int pagetbl_seq_show(struct seq_file *s, void *v)
{
        seq_printf(s, "dumping with printk. yolo.\n");
	int ret = dump_pagetable();
	if(ret){
		seq_printf(s, "Something went wrong, errror %d\n", ret);
	}
        return 0;
}


static int pagetbl_open(struct inode *inode, struct file *file)
{
	return single_open(file, pagetbl_seq_show, NULL);
}

static const struct file_operations pagetbl_file_ops = {
        .owner   = THIS_MODULE,
        .open    = pagetbl_open,
        .read    = seq_read,
        .llseek  = seq_lseek,
        .release = single_release // not seq_release because we use single_open
};


struct dentry *pagetbl_debug_root;

static int __init test_module_init(void)
{
	pr_warn("Hello, world\n");
	pagetbl_debug_root = debugfs_create_dir("corny", NULL);
	if (!pagetbl_debug_root)
		return -ENOMEM;

	if (!debugfs_create_file("intel_pagetables", 0444, pagetbl_debug_root, NULL, &pagetbl_file_ops))
		goto fail;

	return 0;
fail:
	debugfs_remove_recursive(pagetbl_debug_root);
	return -ENOMEM;
}
module_init(test_module_init);

static void __exit test_module_exit(void)
{
	pr_warn("Goodbye\n");
	debugfs_remove_recursive(pagetbl_debug_root);
}

module_exit(test_module_exit);

MODULE_AUTHOR("Cornelius Diekmann");
MODULE_LICENSE("GPL");
