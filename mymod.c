#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/init.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/debugfs.h>

#ifndef __x86_64__
  #error "wrong arch"
#endif

#define CORNY_ASSERT(cond) do { if (!(cond)) {printk("Assertion failed\n");} } while (0)

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


static int dump_pagetable(void)
{
	/* https://software.intel.com/sites/default/files/managed/39/c5/325462-sdm-vol-1-2abcd-3abcd.pdf pp.2783
	 * March 2017, version 062 */

	/*Chap 3.2.1 64-Bit Mode Execution Environment. Control registers expand to 64 bits*/
	u64 cr0, cr3, cr4 = 0;
	u64 ia32_efer = 0;



	BUILD_BUG_ON(sizeof(struct page_table) != 4096);
	BUILD_BUG_ON(sizeof(void *) != 8);
	BUILD_BUG_ON(PAGE_SIZE != 4096);
	BUILD_BUG_ON(sizeof(pteval_t) != sizeof(u64));

	pr_warn("Hello, world\n");
	printk("(%s) x86_phys_bits %hu\n", boot_cpu_data.x86_vendor_id, boot_cpu_data.x86_phys_bits);

	if(boot_cpu_data.x86_phys_bits != 36){
		printk("MAXPHYADDR should usually be 36 but is is %u\n", boot_cpu_data.x86_phys_bits);
	}

	BUILD_BUG_ON(sizeof(unsigned long) != sizeof(u64)); // bitmap API uses unsigned long, I want u64. HACK HACK
	bitmap_fill((unsigned long*)&pte_reserved_flags, 51 - boot_cpu_data.x86_phys_bits + 1);
	pte_reserved_flags <<= boot_cpu_data.x86_phys_bits;

	__asm__ __volatile__ ("mov %%cr0, %%rax \n mov %%rax,%0": "=m" (cr0) : /*InputOperands*/ : "rax");
	__asm__ __volatile__ ("mov %%cr3, %%rax \n mov %%rax,%0": "=m" (cr3) : /*InputOperands*/ : "rax");
	__asm__ __volatile__ ("mov %%cr4, %%rax \n mov %%rax,%0": "=m" (cr4) : /*InputOperands*/ : "rax");

	if(rdmsrl_safe(MSR_EFER, &ia32_efer)){
		printk("Error reading MSR\n");
		return 1;
	}


	printk("cr3: 0x%llx\n", cr3);
	if(cr3 & 0xfe7 /*ignored*/
	   || (cr3 & (0xffffffffffffffffLL << boot_cpu_data.x86_phys_bits) /*reserved*/)){
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

	struct page_table *pml4 = phys_to_virt(cr3);
	printk("page table in virtual memory at %p\n", pml4);
	if(!virt_addr_valid(pml4) || !IS_ALIGNED(cr3, 4096)){
		printk("invalid addr!\n");
		return 1; /*error*/
	}

	phys_addr_t pdpt_addr;
	struct page_table *pdpt;
	struct page_table *pd;

	int i,j;
	u64 bm; //bitmap
	u64 addr, addr_max;
	//walk the outermost page table
	for(i = 0; i < 512; ++i){
		u64 e = (u64)pml4->entry[i];
		CORNY_ASSERT(check_entry(e));
		if(!(e & _PAGE_PRESENT)){
			/*skip page which is marked not present*/
			continue;
		}
		printk("entry %p\n", pml4->entry[i]);
		addr = i;
		addr <<= 39; //bits 39:47 (inclusive)
		addr_max = 0x7fffffffffLL; //2**39-1
		if(addr & (1LL << 47) /*highest bit set*/){
			CORNY_ASSERT((addr & 0xffffLL<<48) == 0);
			CORNY_ASSERT((addr_max & 0xffffLL<<48) == 0);
			addr |= (0xffffLL << 48);
		}
		addr_max |= addr;
		printk("v %p %p %s %s %s %s %s\n",
			(void*)addr, (void*)addr_max,
			e & _PAGE_RW ? "W" : "R",
			e & _PAGE_USER ? "U" : "K" /*kernel*/,
			e & _PAGE_PWT ? "PWT" : "",
			e & _PAGE_PCD ? "PCD" : "",
			e & _PAGE_ACCESSED ? "A" : ""
			/*TODO: NX at pos 63, if enabled*/
			);

		/*phsical address of next page table level*/
		bitmap_fill((unsigned long*)&bm, 51 - 12 + 1);
		bm <<= 12;
		pdpt_addr = e & bm;

		pdpt = phys_to_virt(pdpt_addr);
		//printk("pdpt in virtual memory at %p\n", pdpt);
		if(!virt_addr_valid(pdpt) || !IS_ALIGNED(pdpt_addr, 4096)){
			printk("pdpt invalid addr!\n");
			return 1; /*error*/
		}
		for(j = 0; j < 512; ++j){
			u64 addr_pdpt, addr_pdpt_max;
			u64 e = (u64)pdpt->entry[j];
			CORNY_ASSERT(check_entry(e));
			if(!(e & _PAGE_PRESENT)){
				/*skip page which is marked not present*/
				continue;
			}
			//printk("  entry %p\n", pdpt->entry[j]);
			addr_pdpt = j;
			addr_pdpt <<= 30; //bits 30:38 (inclusive)
			CORNY_ASSERT((addr & addr_pdpt) == 0);//no overlapping bits
			addr_pdpt |= addr;
			addr_pdpt_max = 0x3fffffffLL; //2**30-1
			addr_pdpt_max |= addr_pdpt;
			printk("  v %p %p %s %s %s %s %s\n",
				(void*)addr_pdpt, (void*)addr_pdpt_max,
				e & _PAGE_RW ? "W" : "R",
				e & _PAGE_USER ? "U" : "K" /*kernel*/,
				e & _PAGE_PWT ? "PWT" : "",
				e & _PAGE_PCD ? "PCD" : "",
				e & _PAGE_ACCESSED ? "A" : ""
				/*TODO: NX at pos 63, if enabled*/
				);
			if(e & _PAGE_PSE){
				printk("1GB page\n");
				continue;
			}
		}
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
