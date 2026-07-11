#include "vm_functions.h"

#include <linux/printk.h>
#include <asm/barrier.h>

static int vm_function_printint(int n) {
    if (n < INT_MIN || n > INT_MAX) return -1;

    printk(KERN_INFO "%d\n", n);
    return 0;
}

static void vm_function_push_k2u(long long value) {
    unsigned int next_head = (k2u_rb->head + 1) % RING_BUFFER_CAPACITY;
    
    if (next_head != k2u_rb->tail) {
        k2u_rb->data[k2u_rb->head] = value;
        smp_wmb();
        k2u_rb->head = next_head;
    }
}

static long long vm_function_pop_u2k(void) {
    if (u2k_rb->head == u2k_rb->tail) return 0;

    long long val = u2k_rb->data[u2k_rb->tail];
    smp_rmb();
    
    u2k_rb->tail = (u2k_rb->tail + 1) % RING_BUFFER_CAPACITY;
    return val;
}

void* vm_functions[VM_FUNCTIONS_COUNT] = {
    [VM_FUNCTION_PRINTINT] = (void*)vm_function_printint,
    [VM_FUNCTION_PUSH_RING_BUFFER_K2U] = (void*)vm_function_push_k2u,
    [VM_FUNCTION_POP_RING_BUFFER_U2K] = (void*)vm_function_pop_u2k
};
