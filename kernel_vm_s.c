#include "kernel_vm_s.h"
#include "jit_compiler_x86_64.h"

#include <linux/module.h>
#include <linux/execmem.h>
#include <linux/kprobes.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/version.h>

#define CLASS_NAME "vm_s_c"

static struct page *k2u_page, *u2k_page;
struct rb_to_user *k2u_rb = NULL;
struct rb_to_kernel *u2k_rb = NULL;

struct user_jit_function {
    unsigned char* bin_code;
    int len;
    struct rcu_head rcu;
};

struct radix_tree_root user_jit_functions_tree;
static DEFINE_SPINLOCK(user_jit_functions_lock);
RADIX_TREE(user_jit_functions, GFP_KERNEL);

static int major_number;
static struct class* char_class = NULL;
static struct device* char_device = NULL;

void* (*kexport_execmem_alloc)(enum execmem_type type, size_t size) = NULL;
void  (*kexport_execmem_free)(void *p) = NULL;
void* (*kexport_text_poke)(void *addr, const void *opcode, size_t len) = NULL;

static int dev_open(struct inode *, struct file *);
static int dev_release(struct inode *, struct file *);
static ssize_t dev_read(struct file *, char *, size_t, loff_t *);
static ssize_t dev_write(struct file *, const char *, size_t, loff_t *);
static int dev_mmap_ring_buffer(struct file *, struct vm_area_struct *);

static struct file_operations fops = {
    .open = dev_open,
    .read = dev_read,
    .write = dev_write,
    .release = dev_release,
    .mmap = dev_mmap_ring_buffer
};

int _vm_packet_handler(const struct dev_vm_packet*);

static int dev_mmap_ring_buffer(struct file *filp, struct vm_area_struct *vma) {
    unsigned long size = vma->vm_end - vma->vm_start;
    unsigned long pfn;

    if (vma->vm_pgoff == 0) {
        pfn = page_to_pfn(k2u_page);
    } else if (vma->vm_pgoff == 1) {
        pfn = page_to_pfn(u2k_page);
    } else {
        return -EINVAL;
    }

    return remap_pfn_range(vma, vma->vm_start, pfn, size, vma->vm_page_prot);
}

static int save_jit_function(unsigned long id, unsigned char* bin_code, int size)
{
    struct user_jit_function *desc;
    int err;

    desc = kmalloc(sizeof(*desc), GFP_KERNEL);
    if (!desc)
        return -ENOMEM;

    desc->bin_code = bin_code;
    desc->len = size;

    err = radix_tree_preload(GFP_KERNEL);
    if (err) {
        kfree(desc);
        return err;
    }

    spin_lock(&user_jit_functions_lock);

    err = radix_tree_insert(&user_jit_functions, id, desc);

    spin_unlock(&user_jit_functions_lock);
    radix_tree_preload_end();

    if (err == -EEXIST) {
        kfree(desc);
        return err;
    }

    return 0;
}

static int _delete_jit_function(unsigned long id)
{
    struct user_jit_function *desc;

    spin_lock(&user_jit_functions_lock);
    desc = radix_tree_delete(&user_jit_functions, id);
    spin_unlock(&user_jit_functions_lock);

    if (!desc) {
        pr_warn("VM: Attempted to delete non-existent JIT function %lu\n", id);
        return -ENOENT;
    }

    synchronize_rcu();

    if (desc->bin_code && kexport_execmem_free) {
        kexport_execmem_free(desc->bin_code);
    }
    
    kfree(desc);

    pr_info("VM: JIT function %lu successfully deleted from kernel\n", id);
    return 0;
}

static void clear_all_jit_functions(void) {
    struct user_jit_function *desc;
    struct radix_tree_iter iter;
    void __rcu **slot;

    pr_info("VM: Starting final memory cleanup...\n");

    spin_lock(&user_jit_functions_lock);
    
    radix_tree_for_each_slot(slot, &user_jit_functions, &iter, 0) {
        desc = radix_tree_deref_slot_protected(slot, &user_jit_functions_lock);
        
        if (desc) {
            radix_tree_delete(&user_jit_functions, iter.index);
            
            if (desc->bin_code && kexport_execmem_free) {
                kexport_execmem_free(desc->bin_code);
            }
            kfree(desc);
            
            pr_info("VM: Cleaned up JIT function at slot %lu\n", iter.index);
        }
    }

    spin_unlock(&user_jit_functions_lock);
}

static int devc_create(void) {
    major_number = register_chrdev(0, DEV_VM_NAME, &fops);
    if (major_number < 0) {
        printk(KERN_ALERT "MyCharDev: error register major number\n");
        return major_number;
    }

    char_class = class_create(CLASS_NAME);
    if (IS_ERR(char_class)) {
        unregister_chrdev(major_number, DEV_VM_NAME);
        return PTR_ERR(char_class);
    }

    char_device = device_create(char_class, NULL, MKDEV(major_number, 0), NULL, DEV_VM_NAME);
    if (IS_ERR(char_device)) {
        class_destroy(char_class);
        unregister_chrdev(major_number, DEV_VM_NAME);
        return PTR_ERR(char_device);
    }

    printk(KERN_INFO "DevC: Device successfully created /dev/%s\n", DEV_VM_NAME);
    return 0;
}

static void devc_unregister(void) {
    device_destroy(char_class, MKDEV(major_number, 0));
    class_destroy(char_class);
    unregister_chrdev(major_number, DEV_VM_NAME);
    printk(KERN_INFO "DevC: Module unregister\n");
}

static int dev_open(struct inode *inod, struct file *fil) {
    return 0;
}

static ssize_t dev_read(struct file *fil, char *buffer, size_t len, loff_t *offset) {
    return 0;
}

static ssize_t dev_write(struct file *fil, const char *buffer, size_t len, loff_t *offset) {
    struct dev_vm_packet packet;
    unsigned char* kernel_array = NULL;
    int packet_handler_result;

    if (len < sizeof(struct dev_vm_packet)) {
        pr_err("VM_C: Invalid write length %zu (expected at least %zu)\n", len, sizeof(struct dev_vm_packet));
        return -EINVAL;
    }
    
    if (copy_from_user(&packet, buffer, sizeof(struct dev_vm_packet))) {
        return -EFAULT;
    }

    if (packet.cmd == DEV_VM_PACKET_CMD_WRITE) {
        if (packet.data_size == 0 || packet.data_size > (VM_COMPLEXITY_LIMIT_INSTRUCTIONS * sizeof(struct instruction))) {
            return -EINVAL;
        }

        kernel_array = kmalloc(packet.data_size, GFP_KERNEL);
        if (!kernel_array) {
            return -ENOMEM;
        }

        if (copy_from_user(kernel_array, packet.data, packet.data_size)) {
            kfree(kernel_array);
            return -EFAULT;
        }

        packet.data = kernel_array;
    } else {
        packet.data = NULL;
        packet.data_size = 0;
    }

    packet_handler_result = _vm_packet_handler(&packet);

    if (kernel_array) {
        kfree(kernel_array);
    }

    return (packet_handler_result < 0) ? packet_handler_result : len;
}

static int dev_release(struct inode *inod, struct file *fil) {
    return 0;
}

static int _compile_and_save_vm_bytecode(unsigned long id, const void* bytecode, size_t bytecode_size) {
    int instr_count = bytecode_size / sizeof(struct instruction);
    int verif_status, jit_status, err;
    unsigned char* code_bytecode = NULL;
    void* exec_mem = NULL;
    size_t alloc_size;

    verif_status = verificator((struct instruction*)bytecode, instr_count);
    if (verif_status < 0) {
        return verif_status;
    }

    code_bytecode = (unsigned char*)kvzalloc(VM_COMPLEXITY_LIMIT_INSTRUCTIONS * 8, GFP_KERNEL);
    if (!code_bytecode) {
        return -ENOMEM;
    }

    jit_status = jit_x86_64_compiler((struct instruction*)bytecode, instr_count,
                                    code_bytecode, VM_COMPLEXITY_LIMIT_INSTRUCTIONS * 8);
    if (jit_status < 0) {
        kvfree(code_bytecode);
        return jit_status;
    }

    alloc_size = PAGE_ALIGN(jit_status);
    exec_mem = kexport_execmem_alloc(EXECMEM_MODULE_TEXT, alloc_size);
    if (!exec_mem) {
        kvfree(code_bytecode);
        return -ENOMEM;
    }
    kexport_text_poke(exec_mem, code_bytecode, jit_status);
    kvfree(code_bytecode);

    err = save_jit_function(id, exec_mem, jit_status);
    if (err < 0) {
        if (kexport_execmem_free) kexport_execmem_free(exec_mem);
        return err;
    }

    pr_info("VM: JIT function %lu compiled and saved successfully\n", id);
    return 0;
}

static int _execute_saved_vm_bytecode(unsigned long id) {
    struct user_jit_function *desc;
    int vm_result;

    rcu_read_lock();
    
    desc = radix_tree_lookup(&user_jit_functions, id);
    if (!desc) {
        rcu_read_unlock();
        pr_warn("VM: JIT function %lu not found for execution\n", id);
        return -ENOENT;
    }

    vm_result = ((int (*)(void))desc->bin_code)();

    rcu_read_unlock();

    pr_info("VM: JIT execution %lu finished with status: %d\n", id, vm_result);
    return vm_result;
}

int _vm_packet_handler(const struct dev_vm_packet* packet) {
    switch (packet->cmd) {
    case DEV_VM_PACKET_CMD_WRITE:
        return _compile_and_save_vm_bytecode(packet->id, packet->data, packet->data_size);

    case DEV_VM_PACKET_CMD_EXECUTE:
        return _execute_saved_vm_bytecode(packet->id);

    case DEV_VM_PACKET_CMD_DELETE:
        return _delete_jit_function(packet->id);

    default:
        pr_warn("VM: Unknown command %d\n", packet->cmd);
        return -EINVAL;
    }
}

static unsigned long lookup_symbol(const char *name) {
    struct kprobe kp = { .symbol_name = name };
    unsigned long retval;

    if (register_kprobe(&kp) < 0) return 0;
    retval = (unsigned long)kp.addr;
    unregister_kprobe(&kp);
    return retval;
}

static int __init vm_init(void) {
#ifndef CONFIG_X86_64
    pr_err("VM ERROR: This module supports ONLY x86_64 architecture!\n");
    return -ENODEV;
#endif

    if (LINUX_VERSION_CODE < KERNEL_VERSION(6, 4, 0)) {
        pr_err("VM ERROR: Kernel version %d.%d.%d is too old. Requires >= 6.4.0.\n",
               (LINUX_VERSION_CODE >> 16) & 0xFF,
               (LINUX_VERSION_CODE >> 8) & 0xFF,
               LINUX_VERSION_CODE & 0xFF);
        return -ENOTSUPP;
    }

    int devc_create_result = devc_create();
    if (devc_create_result != 0) {
        return devc_create_result;
    }

    kexport_execmem_alloc = (void *)lookup_symbol("execmem_alloc");
    kexport_execmem_free = (void *)lookup_symbol("execmem_free");
    kexport_text_poke = (void *)lookup_symbol("text_poke");

    if (!kexport_execmem_alloc || !kexport_text_poke || !kexport_execmem_free) {
        printk(KERN_ERR "VM ERROR: Failed to resolve required internal kernel symbols!\n");
        devc_unregister();
        return -EINVAL;
    }

    k2u_page = alloc_pages(GFP_KERNEL | __GFP_ZERO, 0);
    u2k_page = alloc_pages(GFP_KERNEL | __GFP_ZERO, 0);
    
    k2u_rb = page_address(k2u_page);
    u2k_rb = page_address(u2k_page);

    printk(KERN_INFO "VM Module initialized successfully.\n");
    return 0;
}

static void __exit vm_exit(void)  {
    devc_unregister();

    clear_all_jit_functions();

    if (k2u_page) {
        __free_pages(k2u_page, 0);
        k2u_rb = NULL;
        k2u_page = NULL;
    }

    if (u2k_page) {
        __free_pages(u2k_page, 0);
        u2k_rb = NULL;
        u2k_page = NULL;
    }

    printk(KERN_INFO "VM Module: Module unregistered.\n");
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("A1ex3");
MODULE_DESCRIPTION("Simple VM x86_64");

module_init(vm_init);
module_exit(vm_exit);