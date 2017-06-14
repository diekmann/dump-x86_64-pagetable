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
#define X86_CR4_PKE_BIT		22 /*Protection Keys support */
#define X86_CR4_PKE		_BITUL(X86_CR4_PKE_BIT)
#endif

#define CORNY_ASSERT(cond) do { if (!(cond)) {printk("Assertion failed: "#cond" at %s, line %d.\n", __FILE__, __LINE__);} } while (0)

static inline u64 bitmask_numbits(int numbits)
{
	return (1LL << numbits) - 1;
}


/* A 4k intel page table with 512 64bit entries. */
struct page_table{
	void* entry[512];
};


/* Valid intel page sizes*/
char* string_page_size(int bitpos)
{
	size_t pagesize;
	CORNY_ASSERT(bitpos == 30 || bitpos == 21 || bitpos == 12);
	pagesize = 1 << bitpos;
	switch(pagesize){
		case 1024*1024*1024:
			return "1GB";
		case 2*1024*1024:
			return "2MB";
		case 4*1024:
			return "4KB";
		default:
			return "<BUG PAGESIZE>";
	}
}

/* page table bits 51:M reserved, must be 0. Create global bitmask once.*/
static u64 pte_reserved_flags;


static int check_entry(u64 e){
	/* 51:M reserved, must be 0*/
	if(e & pte_reserved_flags){
		printk("invalid entry!\n");
		return 0;
	}
	if(e & _PAGE_PSE){
		//TODO references a page directly, probably sanity check address?
	}
	if(!(e & _PAGE_PRESENT) && e){
		printk("strange entry!\n");
		return 1;
	}
	return 1;
}

struct dumptbl_state {
	int maxphyaddr;

	struct page_table *pml4; /*pointer to pml4 page table in virtual memory*/
	int pml4_i; /*current index into the pml4, points to a pml4e (pml4 entry)*/
	u64 pml4_baddr; /*virtual memory base address mapped by current pml4e*/

	struct page_table *pdpt;
	int pdpt_i;
	u64 pdpt_baddr;

	struct page_table *pd;
	int pd_i;
	u64 pd_baddr;
};

/*The part of the virtual address defined by the page table entry at index for the page table level indicated by bitpos*/
static inline u64 pte_addr_part(int index, int bitpos)
{
	CORNY_ASSERT(index < 512);
	CORNY_ASSERT(bitpos == 39 || bitpos == 30 || bitpos == 21 || bitpos == 12);
	return ((u64)index) << bitpos;
}

/* bitpos: The position of the bit of the virtual memory address that the current page table level refers to.
 * PML4 39:47 (inclusive)
 * PDPT 30:38 (inclusive)
 * PD   21:29 (inclusive)
 *
 * returns:
 *  1: descend to deeper levels
 *  0: stop descending
 * */
int dump_entry(struct seq_file *s, struct dumptbl_state *state, int bitpos)
{
	char *str_level;
	struct page_table *table; //page table
	int i; //index into the table
	u64 e; //page table entry 

	u64 *baddr; //pointer to state struct with base address of current entry. To be set in this function.
	u64 outer_baddr; //base address of the outer page tables (base address of entry = outer_baddr | baddr)
	u64 addr_max; //maximum virtual address described by the current page table entry


	CORNY_ASSERT(bitpos == 39 || bitpos == 30 || bitpos == 21);
	CORNY_ASSERT(state->pml4_i < 512);
	CORNY_ASSERT(state->pdpt_i < 512);
	CORNY_ASSERT(state->pd_i < 512);

	switch(bitpos){
		case 39: /*PML4*/
			table = state->pml4;
			i = state->pml4_i;
			str_level = "pml4";
			baddr = &state->pml4_baddr;
			//sign extend the most significant bits to canonical address
			outer_baddr = 0;
			if(pte_addr_part(i, 39) & (1LL << 47) /*highest bit set*/){
				outer_baddr = (0xffffLL << 48);
			}
			break;
		case 30: /*PDPT*/
			table = state->pdpt;
			i = state->pdpt_i;
			str_level = "  pdpt";
			outer_baddr = state->pml4_baddr;
			baddr = &state->pdpt_baddr;
			break;
		case 21: /*PD*/
			table = state->pd;
			i = state->pd_i;
			str_level = "      pd";
			CORNY_ASSERT((state->pml4_baddr | state->pdpt_baddr) == state->pdpt_baddr);
			outer_baddr = state->pdpt_baddr;
			baddr = &state->pd_baddr;
			break;
	}

	e = (u64)table->entry[i];


	CORNY_ASSERT(check_entry(e));
	if(!(e & _PAGE_PRESENT)){
		/*skip page which is marked not present*/
		return 0;
	}
	*baddr = outer_baddr | pte_addr_part(i, bitpos);
	CORNY_ASSERT((outer_baddr & pte_addr_part(i, bitpos)) == 0); // no overlapping bits
	CORNY_ASSERT(((state->pml4_baddr | state->pdpt_baddr) & state->pml4_baddr) == state->pml4_baddr); //no overlapping bits
	addr_max = bitmask_numbits(bitpos);
	CORNY_ASSERT((state->pml4_baddr & addr_max) == 0); //no overlapping bits
	CORNY_ASSERT(((state->pdpt_baddr & addr_max) == 0)); //no overlapping bits
	addr_max |= *baddr;
	seq_printf(s, "%s v %p %p %s %s %s %s %s %s", str_level,
		(void*)*baddr, (void*)addr_max,
		e & _PAGE_RW ? "W" : "R",
		e & _PAGE_USER ? "U" : "K" /*kernel*/,
		e & _PAGE_PWT ? "PWT" : "",
		e & _PAGE_PCD ? "PCD" : "",
		e & _PAGE_ACCESSED ? "A" : "",
		e & _PAGE_NX ? "NX" : ""
		);
	if(e & _PAGE_PSE){
		seq_printf(s, " -> %s page\n", string_page_size(bitpos));
		return 0; // do not descend to any deeper page tables!
	}
	seq_printf(s, "\n");
	return 1; //continue
}

void* next_page_table_vaddr(u64 pagetbl_entry)
{
	void *vaddr; //virtual addr of page table entry
	phys_addr_t paddr; //physical addr of page table entry

	/*pagetble_entry bits 51:12 contains the physical address of the next page table level*/
	u64 bm = bitmask_numbits(51 - 12 + 1) << 12;
	paddr = pagetbl_entry & bm;
	/*Actually, 51:.. is too generous, there are some reserved bits which must be zero*/
	CORNY_ASSERT((pagetbl_entry & pte_reserved_flags) == 0);

	vaddr = phys_to_virt(paddr);
	if(!virt_addr_valid(vaddr) || !IS_ALIGNED(paddr, 4096)){
		printk("CRITICAL: invalid addr!\n");
		return NULL; /*error*/
	}
	return vaddr;
}

static int dump_pagetable(struct seq_file *s)
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

	seq_printf(s, "(%s) x86_phys_bits %hu\n", boot_cpu_data.x86_vendor_id, boot_cpu_data.x86_phys_bits);

	if(boot_cpu_data.x86_phys_bits != 36){
		seq_printf(s, "MAXPHYADDR should usually be 36 but is is %u\n", boot_cpu_data.x86_phys_bits);
	}
	state.maxphyaddr = boot_cpu_data.x86_phys_bits;

	pte_reserved_flags = bitmask_numbits(51 - state.maxphyaddr + 1); //bitmap with up to 51 bit set
	pte_reserved_flags <<= state.maxphyaddr;

	__asm__ __volatile__ ("mov %%cr0, %%rax \n mov %%rax,%0": "=m" (cr0) : /*InputOperands*/ : "rax");
	__asm__ __volatile__ ("mov %%cr3, %%rax \n mov %%rax,%0": "=m" (cr3) : /*InputOperands*/ : "rax");
	__asm__ __volatile__ ("mov %%cr4, %%rax \n mov %%rax,%0": "=m" (cr4) : /*InputOperands*/ : "rax");

	if(rdmsrl_safe(MSR_EFER, &ia32_efer)){
		seq_printf(s, "Error reading MSR\n");
		return 1;
	}

	if(!(ia32_efer & EFER_NX)){
		seq_printf(s, "No IA32_EFER.NXE?????\n");
	}


	seq_printf(s, "cr3: 0x%llx\n", cr3);
	if(cr3 & 0xfe7 /*ignored*/
	   || (cr3 & (0xffffffffffffffffLL << state.maxphyaddr) /*reserved*/)){
		seq_printf(s, "cr3 looks shady\n");
	}
	if(cr3 & X86_CR3_PWT || cr3 & X86_CR3_PCD){
		seq_printf(s, "unexpected options in cr3\n");
	}
	if(cr0 & X86_CR0_PG  &&  cr4 & X86_CR4_PAE  &&  ia32_efer & EFER_LME  &&  !(cr4 & X86_CR4_PCIDE)){
		seq_printf(s, "paging according to Tab 4-12 p. 2783 intel dev manual (March 2017)\n");
	}else{
		seq_printf(s, "unknown paging setup\n");
		return -EPERM; /*I'm afraid I can't do that*/
	}
	if(!(cr4 & X86_CR4_PKE)){
		seq_printf(s, "No protection keys enabled (this is normal)\n");
	}

	state.pml4 = phys_to_virt(cr3);
	seq_printf(s, "page table in virtual memory at %p\n", state.pml4);
	if(!virt_addr_valid(state.pml4) || !IS_ALIGNED(cr3, 4096)){
		seq_printf(s, "invalid addr!\n");
		return 1; /*error*/
	}


	//walk the outermost page table
	for(state.pml4_i = 0; state.pml4_i < 512; ++state.pml4_i){
		if(!dump_entry(s, &state, 39)){
			//outer level cannot map a page directly but it can have pages which are nto present
			continue;
		}

		// walk next level
		state.pdpt = next_page_table_vaddr((u64)state.pml4->entry[state.pml4_i]);
		for(state.pdpt_i = 0; state.pdpt_i < 512; ++state.pdpt_i){
			if(dump_entry(s, &state, 30)){
				// walk next level
				state.pd = next_page_table_vaddr((u64)state.pdpt->entry[state.pdpt_i]);
				for(state.pd_i = 0; state.pd_i < 512; ++state.pd_i){
					if(dump_entry(s, &state, 21)){
						//print next levels here
					}
				};
				// reset pd entries in state for assertions
				state.pd = NULL;
				state.pd_i = 0;
				state.pd_baddr = 0;
			}
		};
		// reset pdpt entries in state for assertions
		state.pdpt = NULL;
		state.pdpt_i = 0;
		state.pdpt_baddr = 0;
	}
	return 0;
}

static int pagetbl_seq_show(struct seq_file *s, void *v)
{
        seq_printf(s, "dumping all in one go. yolo.\n");
	int ret = dump_pagetable(s);
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
