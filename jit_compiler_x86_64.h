#ifndef __JIT__COMPILER_X86_64_H__
#define __JIT__COMPILER_X86_64_H__

#include "kernel_vm_s.h"

#define VM_COMPLEXITY_LIMIT_INSTRUCTIONS 100000

int jit_x86_64_compiler(struct instruction*, int, unsigned char*, int);

#endif