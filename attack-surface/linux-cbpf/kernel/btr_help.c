/*
 * Branch Target Reuse (BTR)
 * April 30th 2026
 * Sander Wiebing
 */


#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/stat.h>
#include <linux/slab.h>
#include <linux/proc_fs.h>
#include <linux/delay.h>
#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <linux/highmem.h>


MODULE_AUTHOR("Anonymous");
MODULE_DESCRIPTION("BTR Helper module");
MODULE_LICENSE("GPL");

#define MODULE_MAPPING_START 0xffffffffc0000000 // 0xffffffffc0000000
#define MODULE_MAPPING_END   0xfffffffffeffffff

#define PAGE_2MB 1 << 21 // 2 MB

__always_inline static void cpuid_fence(void) { asm volatile ("xor %%rax, %%rax\ncpuid\n\t" ::: "%rax", "%rbx", "%rcx", "%rdx"); }
__always_inline static void flush(void * addr) { asm volatile ("clflush (%0)\n\t" :: "r"(addr):); }
__always_inline static void mfence(void) { asm volatile ("mfence\n\t":::); }
__always_inline static void lfence(void) { asm volatile ("lfence\n\t":::); }
__always_inline static void sfence(void) { asm volatile ("sfence\n\t":::); }


static __always_inline __attribute__((always_inline)) void maccess(void *p) {
        *(volatile unsigned char *)p;
}

static __always_inline __attribute__((always_inline)) uint64_t rdtscp(void) {
        uint64_t lo, hi;
        asm volatile("rdtscp\n" : "=a" (lo), "=d" (hi) :: "rcx");
        return (hi << 32) | lo;
}


static __always_inline __attribute__((always_inline)) uint64_t load_time(void *p)
{
    uint64_t t0 = rdtscp();
    maccess(p);
    return rdtscp() - t0;
}



extern uint64_t disclosure_gadget(uint64_t * fr_buf);
extern uint64_t disclosure_gadget_constant_blind(uint64_t * fr_buf);


static int is_address_mapped(void *virt_addr)
{
	unsigned long va = (unsigned long)virt_addr;
	unsigned long *r = NULL;
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;

	pgd = pgd_offset(current->mm, va);
	if (pgd_none(*pgd) || pgd_bad(*pgd))
		return 0;

	p4d = p4d_offset(pgd, va);
	if (p4d_none(*p4d) || p4d_bad(*p4d))
		return 0;

	pud = pud_offset(p4d, va);
	if (pud_none(*pud))
		return 0;

	if (pud_bad(*pud))
		return 0;


	pmd = pmd_offset(pud, va);
	if (pmd_none(*pmd))
		return 0;


	if (pmd_trans_huge(*pmd)) {
        pr_info("Huge page!\n");
		return 2;
	}


	if (pmd_bad(*pmd))
		return 0;


	pte = pte_offset_kernel(pmd, va);
	if (!pte_none(*pte)) {
		r = (unsigned long *)pte;
	}

	pte_unmap(pte);

    if (r) {
        return 1;
    }

	return 0;
}


static ssize_t mod_get_mapped_module_regions(struct file *filp, char *buf, size_t len, loff_t *off)
{
    char kbuf[256];
    uint64_t kern_address;
    int write_len = 0;
    int ret;
    int is_huge_page = 0;
    // uint64_t value;

    uint64_t region_start;
    uint64_t region_size;
    uint64_t n_regions = 0;

    if (*off != 0) {
        return 0;
    }

    pr_info("MODULE_MAPPING_START: %lx MODULE_MAPPING_END: %lx\n", MODULE_MAPPING_START, MODULE_MAPPING_END);

    region_start = 0;
    region_size = 0;

    for (kern_address = MODULE_MAPPING_START; kern_address < MODULE_MAPPING_END; kern_address += PAGE_SIZE)
    {
        ret = is_address_mapped((void *) kern_address);
        if (ret != 0) {

            if (region_start + region_size == kern_address) {
                if (ret == 1) {
                    region_size += PAGE_SIZE;
                } else {
                    is_huge_page = 1;
                    region_size += PAGE_2MB;
                }

            } else {
                if (region_start != 0) {
                    n_regions += 1;
                    if (is_huge_page) {
                       pr_info("%llx - %llx  %6llx HUGE\n", region_start, region_start + region_size, region_size);
                    } else {
                       pr_info("%llx - %llx  %llx\n", region_start, region_start + region_size, region_size);
                    }
                }

                // start a new region
                region_start = kern_address;
                if (ret == 1) {
                    is_huge_page = 0;
                    region_size = PAGE_SIZE;
                } else {
                    is_huge_page = 1;
                    region_size = PAGE_2MB;
                }
            }
        }
    }

    if (region_start != 0) {
        n_regions += 1;
        if (is_huge_page) {
            pr_info("%llx - %llx  %6llx HUGE\n", region_start, region_start + region_size, region_size);
        } else {
            pr_info("%llx - %llx  %llx\n", region_start, region_start + region_size, region_size);
        }
    }

    pr_info("Total mapped regions: %llu\n", n_regions);

    kbuf[0] = 'X';
    kbuf[1] = '\n';
    kbuf[2] = '\0';
    write_len = min(len, strlen(kbuf));
    *off += write_len;

    if (copy_to_user(buf, kbuf, write_len)) {
        return -EFAULT;
    }

	return write_len;


}

static ssize_t mod_get_last_module_region(struct file *filp, char *buf, size_t len, loff_t *off)
{
    char kbuf[256];
    uint64_t kern_address;
    int write_len = 0;
    int ret;
    int is_huge_page = 0;
    // uint64_t value;

    uint64_t region_start;
    uint64_t region_size;

    if (*off != 0) {
        return 0;
    }

    region_start = 0;
    region_size = 0;

    for (kern_address = MODULE_MAPPING_START; kern_address < MODULE_MAPPING_END; kern_address += PAGE_SIZE)
    {
        ret = is_address_mapped((void *) kern_address);
        if (ret != 0) {

            if (region_start + region_size == kern_address) {
                if (ret == 1) {
                    region_size += PAGE_SIZE;
                } else {
                    is_huge_page = 1;
                    region_size += PAGE_2MB;
                }

            } else {

                // start a new region
                region_start = kern_address;
                if (ret == 1) {
                    is_huge_page = 0;
                    region_size = PAGE_SIZE;
                } else {
                    is_huge_page = 1;
                    region_size = PAGE_2MB;
                }
            }
        }
    }


    snprintf(kbuf, 64, "0x%llx 0x%llx\n", region_start, region_size);

    write_len = min(len, strlen(kbuf));
    *off += write_len;

    if (copy_to_user(buf, kbuf, write_len)) {
        return -EFAULT;
    }

	return write_len;


}


static ssize_t mod_get_phys_map_start(struct file *filp, char *buf, size_t len, loff_t *off)
{
    char kbuf[256];
    int write_len = 0;

    if (*off != 0) {
        return 0;
    }

    snprintf(kbuf, 32, "%llx\n", (uint64_t) page_offset_base);

    write_len = min(len, strlen(kbuf));
    *off += write_len;

    if (copy_to_user(buf, kbuf, write_len)) {
        return -EFAULT;
    }

	return write_len;


}


static ssize_t mod_mock_gadget_address(struct file *filp, char *buf, size_t len, loff_t *off)
{
    char kbuf[256];
    int write_len = 0;

    if (*off != 0) {
        return 0;
    }

    snprintf(kbuf, 64, "0x%llx\n", (uint64_t) disclosure_gadget);

    write_len = min(len, strlen(kbuf));
    *off += write_len;

    if (copy_to_user(buf, kbuf, write_len)) {
        return -EFAULT;
    }

	return write_len;
}

static ssize_t mod_mock_gadget_address_cb(struct file *filp, char *buf, size_t len, loff_t *off)
{
    char kbuf[256];
    int write_len = 0;

    if (*off != 0) {
        return 0;
    }

    snprintf(kbuf, 64, "0x%llx\n", (uint64_t) disclosure_gadget_constant_blind);

    write_len = min(len, strlen(kbuf));
    *off += write_len;

    if (copy_to_user(buf, kbuf, write_len)) {
        return -EFAULT;
    }

	return write_len;
}



uint8_t * prog_struct_p = 0;


static ssize_t mod_get_and_set_prog_struct(struct file *filp, char *buf, size_t len, loff_t *off)
{
    char kbuf[256];
    int write_len = 0;
    uint64_t value;

    if (*off != 0) {
        return 0;
    }

    // pr_info("current: %px filter: %px\n", current, current->seccomp.filter);

    uint8_t * ptr =  (uint8_t *) current->seccomp.filter;
    ptr += 0x98;

    if (get_kernel_nofault(value, (uint64_t *) ptr)) {
        pr_info("Accessing filter pointer failed: %px\n", ptr);
        return -EFAULT;
    }
    value += 0x30;

    pr_info("prog*: %px\n", (uint8_t *)  value);
    prog_struct_p = (uint8_t *) value;

    if (get_kernel_nofault(value, (uint64_t *) prog_struct_p)) {
        pr_info("Accessing filter pointer failed: %px\n", prog_struct_p);
        return -EFAULT;
    }
    pr_info("cbpf entry: %px\n", (uint8_t *)  value);

    snprintf(kbuf, 32, "%llx\n", (uint64_t) prog_struct_p);

    write_len = min(len, strlen(kbuf));
    *off += write_len;

    if (copy_to_user(buf, kbuf, write_len)) {
        return -EFAULT;
    }

	return write_len;


}

uint64_t last_entry_point = 0;


static ssize_t mod_get_and_set_filter_entry_point(struct file *filp, char *buf, size_t len, loff_t *off)
{
    char kbuf[256];
    int write_len = 0;
    uint64_t value;
    uint64_t entry_point;

    if (*off != 0) {
        return 0;
    }

    uint8_t * ptr =  (uint8_t *) current->seccomp.filter;
    ptr += 0x98;

    if (get_kernel_nofault(value, (uint64_t *) ptr)) {
        pr_info("Accessing filter pointer failed: %px\n", ptr);
        return -EFAULT;
    }
    prog_struct_p = (uint8_t *) value + 0x30;

    if (get_kernel_nofault(entry_point, (uint64_t *) prog_struct_p)) {
        pr_info("Accessing filter pointer failed: %px\n", prog_struct_p);
        return -EFAULT;
    }

    last_entry_point = entry_point;

    snprintf(kbuf, 32, "%llx\n", (uint64_t) entry_point);

    write_len = min(len, strlen(kbuf));
    *off += write_len;

    if (copy_to_user(buf, kbuf, write_len)) {
        return -EFAULT;
    }

	return write_len;


}

static ssize_t mod_read_bytes_old_entry_point(struct file *filp, char *buf, size_t len, loff_t *off)
{
    char kbuf[256];
    int write_len = 0;
    uint64_t value;

    if (*off != 0) {
        return 0;
    }


    // if(!virt_addr_valid(last_entry_point)) {`
    //     pr_info("ERROR: entry-point is invalid (%px)\n", last_entry_point);
    //     return -EFAULT;
    // }

    if (get_kernel_nofault(value, (uint64_t *) last_entry_point)) {
        pr_info("ERROR: entry-point is not mapped (%px)\n", (void *) last_entry_point);
        return -EFAULT;
    }


    snprintf(kbuf, 32, "%llx\n", (uint64_t) value);

    write_len = min(len, strlen(kbuf));
    *off += write_len;

    if (copy_to_user(buf, kbuf, write_len)) {
        return -EFAULT;
    }

	return write_len;


}

static ssize_t mod_evict_prog_struct(struct file *filp, char *buf, size_t len, loff_t *off)
{
    char kbuf[256];
    int write_len = 0;

    if (*off != 0) {
        return 0;
    }

#if 1
    if (!prog_struct_p) {
        pr_info("ERROR: Prog pointer address is invalid (%px)\n", prog_struct_p);
        return -EFAULT;
    }
#else
    uint8_t * ptr =  (uint8_t *) current->seccomp.filter;
    ptr += 0x98;

    if (get_kernel_nofault(value, (uint64_t *) ptr)) {
        pr_info("Accessing filter pointer failed: %px\n", ptr);
        return -EFAULT;
    }
    value += 0x30;
    prog_struct_p = value;

#endif
    flush(prog_struct_p);
    sfence();

    strncpy(kbuf, "0\n", 32);

    write_len = min(len, strlen(kbuf));
    *off += write_len;

    if (copy_to_user(buf, kbuf, write_len)) {
        return -EFAULT;
    }

	return write_len;

}

static ssize_t mod_time_prog_struct(struct file *filp, char *buf, size_t len, loff_t *off)
{
    char kbuf[256];
    int write_len = 0;
    uint64_t t;

    if (*off != 0) {
        return 0;
    }


    // if(!virt_addr_valid(filter_p)) {
    //     pr_info("ERROR: Pointer address is invalid (%px)\n", filter_p);
    //     return -EFAULT;
    // }

    if (!prog_struct_p) {
        pr_info("ERROR: Prog pointer address is invalid (%px)\n", prog_struct_p);
        return -EFAULT;
    }

    // cpuid_fence();
    t = load_time(prog_struct_p);

    snprintf(kbuf, 32, "%lld\n", (uint64_t) t);

    write_len = min(len, strlen(kbuf));
    *off += write_len;

    if (copy_to_user(buf, kbuf, write_len)) {
        return -EFAULT;
    }

	return write_len;

}



uint8_t * filter_p = 0;

static ssize_t mod_get_and_set_filter(struct file *filp, char *buf, size_t len, loff_t *off)
{
    char kbuf[256];
    int write_len = 0;
    uint64_t value;

    if (*off != 0) {
        return 0;
    }

    pr_info("current: %px filter: %px\n", current, current->seccomp.filter);

    uint8_t * ptr =  (uint8_t *) current->seccomp.filter;
    ptr += 0x98;

    if (get_kernel_nofault(value, (uint64_t *) ptr)) {
        pr_info("Accessing filter pointer failed: %px\n", ptr);
        return -EFAULT;
    }
    value += 0x30;

    pr_info("prog*: %px\n", (uint8_t *)  value);

    filter_p = ptr;
    // filter_p = (uint8_t *) value;

    snprintf(kbuf, 32, "%llx\n", (uint64_t) filter_p);

    write_len = min(len, strlen(kbuf));
    *off += write_len;

    if (copy_to_user(buf, kbuf, write_len)) {
        return -EFAULT;
    }

	return write_len;


}

static ssize_t mod_evict_filter_struct(struct file *filp, char *buf, size_t len, loff_t *off)
{
    char kbuf[256];
    int write_len = 0;
    uint64_t value;

    if (*off != 0) {
        return 0;
    }


#if 0
    if(!virt_addr_valid(filter_p)) {
        pr_info("ERROR: Pointer address is invalid (%px)\n", filter_p);
        return -EFAULT;
    }
#else
    uint8_t * ptr =  (uint8_t *) current->seccomp.filter;
    ptr += 0x98;

    // uint8_t * ptr =  (uint8_t *) current;
    // ptr += 0xcb0;

    if (get_kernel_nofault(value, (uint64_t *) ptr)) {
        pr_info("Accessing filter pointer failed: %px\n", ptr);
        return -EFAULT;
    }

    filter_p = ptr;

    // pr_info("Evicting: %px", filter_p);

#endif
    flush(filter_p);
    sfence();

    // pr_info("Evicting: %px", filter_p);

    strncpy(kbuf, "0\n", 32);

    write_len = min(len, strlen(kbuf));
    *off += write_len;

    if (copy_to_user(buf, kbuf, write_len)) {
        return -EFAULT;
    }

	return write_len;

}

static ssize_t mod_time_filter_struct(struct file *filp, char *buf, size_t len, loff_t *off)
{
    char kbuf[256];
    int write_len = 0;
    uint64_t t;

    if (*off != 0) {
        return 0;
    }


    if (!filter_p) {
        pr_info("ERROR: Filter pointer address is invalid (%px)\n", filter_p);
        return -EFAULT;
    }

    // if(!virt_addr_valid(filter_p)) {
    //     pr_info("ERROR: Pointer address is invalid (%px)\n", filter_p);
    //     return -EFAULT;
    // }

    // cpuid_fence();
    t = load_time(filter_p);

    snprintf(kbuf, 32, "%lld\n", (uint64_t) t);

    write_len = min(len, strlen(kbuf));
    *off += write_len;

    if (copy_to_user(buf, kbuf, write_len)) {
        return -EFAULT;
    }

	return write_len;

}



static struct proc_ops get_mapped_module_regions_fops = {
	.proc_read = mod_get_mapped_module_regions
};

static struct proc_ops get_last_module_region_fops = {
	.proc_read = mod_get_last_module_region
};


static struct proc_ops phys_map_start_fops = {
	.proc_read = mod_get_phys_map_start
};

static struct proc_ops mock_gadget_address_fops = {
	.proc_read = mod_mock_gadget_address
};

static struct proc_ops mock_gadget_address_cb_fops = {
	.proc_read = mod_mock_gadget_address_cb
};

// prog

static struct proc_ops get_and_set_prog_struct_fops = {
	.proc_read = mod_get_and_set_prog_struct
};

static struct proc_ops evict_prog_struct_fops = {
	.proc_read = mod_evict_prog_struct
};

static struct proc_ops time_prog_struct_fops = {
	.proc_read = mod_time_prog_struct
};


// filter

static struct proc_ops get_and_set_filter_struct_fops = {
	.proc_read = mod_get_and_set_filter
};

static struct proc_ops evict_filter_struct_fops = {
	.proc_read = mod_evict_filter_struct
};

static struct proc_ops time_filter_struct_fops = {
	.proc_read = mod_time_filter_struct
};

static struct proc_ops get_and_set_filter_entry_point_fops = {
	.proc_read = mod_get_and_set_filter_entry_point
};

static struct proc_ops read_bytes_old_entry_point_fops = {
	.proc_read = mod_read_bytes_old_entry_point
};


static struct proc_dir_entry *proc_dir;


static int __init btr_help_init(void)
{

    // Create the proc dir
    proc_dir = proc_mkdir("btr_help", NULL);

    proc_create("mapped_module_regions", 0666, proc_dir, &get_mapped_module_regions_fops);
    proc_create("last_mapped_module_region", 0666, proc_dir, &get_last_module_region_fops);

    proc_create("phys_map_start", 0666, proc_dir, &phys_map_start_fops);
    proc_create("mock_gadget_address", 0666, proc_dir, &mock_gadget_address_fops);
    proc_create("mock_gadget_address_cb", 0666, proc_dir, &mock_gadget_address_cb_fops);


    proc_create("get_and_set_filter_struct", 0666, proc_dir, &get_and_set_filter_struct_fops);
    proc_create("evict_filter_struct", 0666, proc_dir, &evict_filter_struct_fops);
    proc_create("time_filter_struct", 0666, proc_dir, &time_filter_struct_fops);

    proc_create("get_and_set_prog_struct", 0666, proc_dir, &get_and_set_prog_struct_fops);
    proc_create("evict_prog_struct", 0666, proc_dir, &evict_prog_struct_fops);
    proc_create("time_prog_struct", 0666, proc_dir, &time_prog_struct_fops);


    proc_create("get_and_set_filter_entry_point", 0666, proc_dir, &get_and_set_filter_entry_point_fops);
    proc_create("read_bytes_old_entry_point", 0666, proc_dir, &read_bytes_old_entry_point_fops);


    pr_info("Initialized\n");
    pr_info("disclosure gadget: %px\n", disclosure_gadget);
    pr_info("disclosure gadget constant-blind: %px\n", disclosure_gadget_constant_blind);


	return 0;
}

static void __exit btr_help_exit(void)
{
	pr_info("exiting\n");
    proc_remove(proc_dir);
}

module_init(btr_help_init);
module_exit(btr_help_exit);
