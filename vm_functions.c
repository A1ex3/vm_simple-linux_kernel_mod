#include "vm_functions.h"

#include <linux/printk.h>

static int vm_function_printint(int n) {
    if (n < INT_MIN || n > INT_MAX) return -1;

    printk(KERN_INFO "%d\n", n);
    return 0;
}

void* vm_functions[VM_FUNCTIONS_COUNT] = {
    [VM_FUNCTION_PRINTINT] = (void*)vm_function_printint,
};
