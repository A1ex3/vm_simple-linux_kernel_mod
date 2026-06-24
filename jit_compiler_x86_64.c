#include "jit_compiler_x86_64.h"
#include "vm_functions.h"

#include <linux/kernel.h>
#include <linux/slab.h>

#define VM_LAST_OP_CODE_INSTR_NUM VM_OPC_EXEC_FUNCTION

struct x86_64 {
    int code;
    int is_ext;
};

static int _check_register_num(unsigned char arg) {
    if (arg < 0 || arg >= MAX_REGS) {
        return 0;
    }
    return 1;
}

static int _check_registers_values(
    unsigned char arg0,
    unsigned char arg1,
    unsigned char arg2,
    int imm,
    int arg0_set,
    int arg1_set,
    int arg2_set,
    int imm_set
) {
    if (!arg0_set && arg0 != 0) {
        return 0;
    }
    if (!arg1_set && arg1 != 0) {
        return 0;
    }
    if (!arg2_set && arg2 != 0) {
        return 0;
    }
    if (!imm_set && imm != 0) {
        return 0;
    }
    return 1;
}

int verificator(struct instruction* instr, int instr_count) {
    int shadow_regs[MAX_REGS] = {0};
    int current_stack_depth = 0;
    int max_stack_depth = 512;

    for (int i = 0; i < instr_count; i++) {
        struct instruction curr_instr = instr[i];

        if (curr_instr.op_code > VM_LAST_OP_CODE_INSTR_NUM || curr_instr.op_code == 0) {
            return VERIFI_ERROR_INCORRECT_OP_CODE;
        }

        if (!_check_register_num(curr_instr.arg0)
            || !_check_register_num(curr_instr.arg1)
            || !_check_register_num(curr_instr.arg2)) return VERIFI_ERROR_REGISTER;

        switch (curr_instr.op_code) {
        case VM_OPC_ADD: {
            if (!_check_registers_values(
                curr_instr.arg0, curr_instr.arg1, curr_instr.arg2, curr_instr.imm,
                1, 1, 1, 0)) return VERIFI_ERROR_REGISTERS_VALUES;

            int a = shadow_regs[curr_instr.arg1];
            int b = shadow_regs[curr_instr.arg2];
            if ((b > 0 && a > INT_MAX - b) || (b < 0 && a < INT_MIN - b)) {
                return VERIFI_ERROR_OUT_OF_SIZE_TYPE;
            }
            shadow_regs[curr_instr.arg0] = a + b;
            break;
        }
        case VM_OPC_SUB: {
            if (!_check_registers_values(
                curr_instr.arg0, curr_instr.arg1, curr_instr.arg2, curr_instr.imm,
                1, 1, 1, 0)) return VERIFI_ERROR_REGISTERS_VALUES;

            int a = shadow_regs[curr_instr.arg1];
            int b = shadow_regs[curr_instr.arg2];
            if ((b > 0 && a < INT_MIN + b) || (b < 0 && a > INT_MAX + b)) {
                return VERIFI_ERROR_OUT_OF_SIZE_TYPE;
            }
            shadow_regs[curr_instr.arg0] = a - b;
            break;
        }
        case VM_OPC_MUL: {
            if (!_check_registers_values(
                curr_instr.arg0, curr_instr.arg1, curr_instr.arg2, curr_instr.imm,
                1, 1, 1, 0)) return VERIFI_ERROR_REGISTERS_VALUES;

            int a = shadow_regs[curr_instr.arg1];
            int b = shadow_regs[curr_instr.arg2];
            if (a > 0) {
                if (b > 0 && a > INT_MAX / b) return VERIFI_ERROR_OUT_OF_SIZE_TYPE;
                if (b < 0 && b < INT_MIN / a) return VERIFI_ERROR_OUT_OF_SIZE_TYPE;
            } else if (a < 0) {
                if (b > 0 && a < INT_MIN / b) return VERIFI_ERROR_OUT_OF_SIZE_TYPE;
                if (b < 0 && b < INT_MAX / a) return VERIFI_ERROR_OUT_OF_SIZE_TYPE;
            }
            shadow_regs[curr_instr.arg0] = a * b;
            break;
        }
        case VM_OPC_DIVIDE: {
            if (!_check_registers_values(
                curr_instr.arg0, curr_instr.arg1, curr_instr.arg2, curr_instr.imm,
                1, 1, 1, 0)) return VERIFI_ERROR_REGISTERS_VALUES;

            int b = shadow_regs[curr_instr.arg2];
            if (b == 0) {
                return VERIFI_ERROR_DIVIDE_ZERO;
            }
            int a = shadow_regs[curr_instr.arg1];
            if (a == INT_MIN && b == -1) {
                return VERIFI_ERROR_OUT_OF_SIZE_TYPE; 
            }
            shadow_regs[curr_instr.arg0] = a / b;
            break;
        }
        case VM_OPC_MOV: {
            if (!_check_registers_values(
                curr_instr.arg0, curr_instr.arg1, curr_instr.arg2, curr_instr.imm,
                1, 0, 0, 1)) return VERIFI_ERROR_REGISTERS_VALUES;

            shadow_regs[curr_instr.arg0] = curr_instr.imm; 
            break;
        }
        case VM_OPC_MOV_REG: {
            if (!_check_registers_values(
                curr_instr.arg0, curr_instr.arg1, curr_instr.arg2, curr_instr.imm,
                1, 1, 0, 0)) return VERIFI_ERROR_REGISTERS_VALUES;

            shadow_regs[curr_instr.arg0] = shadow_regs[curr_instr.arg1];
            break;
        }
        case VM_OPC_JMP: {
            if (!_check_registers_values(
                curr_instr.arg0, curr_instr.arg1, curr_instr.arg2, curr_instr.imm,
                0, 0, 0, 1)) return VERIFI_ERROR_REGISTERS_VALUES;

            int offset = curr_instr.imm;
            if (offset == 0) {
                return VERIFI_ERROR_INFINITE_LOOP; 
            }

            int target_pc = i + offset;
            if (target_pc < 0 || target_pc >= instr_count) {
                return VERIFI_ERROR_OUT_OF_PROGRAMM;
            }
            break;
        }
        case VM_OPC_JEQ:
        case VM_OPC_JNE:
        case VM_OPC_JLT:
        case VM_OPC_JLE:
        case VM_OPC_JGT:
        case VM_OPC_JGE: {
            if (!_check_registers_values(
                curr_instr.arg0, curr_instr.arg1, curr_instr.arg2, curr_instr.imm,
                1, 1, 0, 1)) return VERIFI_ERROR_REGISTERS_VALUES;

            int offset = curr_instr.imm;
            if (offset == 0) {
                return VERIFI_ERROR_INFINITE_LOOP; 
            }

            int target_pc = i + offset;
            if (target_pc < 0 || target_pc >= instr_count) {
                return VERIFI_ERROR_OUT_OF_PROGRAMM;
            }
            break;
        }
        case VM_OPC_EXIT:
            if (!_check_registers_values(
                curr_instr.arg0, curr_instr.arg1, curr_instr.arg2, curr_instr.imm,
                1, 0, 0, 0)) return VERIFI_ERROR_REGISTERS_VALUES;
            break;
        case VM_OPC_EXIT_STATUS_CODE:
            if (!_check_registers_values(
                curr_instr.arg0, curr_instr.arg1, curr_instr.arg2, curr_instr.imm,
                0, 0, 0, 1)) return VERIFI_ERROR_REGISTERS_VALUES;
            break;
        case VM_OPC_EXEC_FUNCTION:
            if (!_check_registers_values(
                curr_instr.arg0, curr_instr.arg1, curr_instr.arg2, curr_instr.imm,
                1, 0, 0, 1)) return VERIFI_ERROR_REGISTERS_VALUES;

            if (curr_instr.imm < 0 || (unsigned)curr_instr.imm >= VM_FUNCTIONS_COUNT)
                return VERIFI_ERROR_FUNCTION_DOESNT_EXISTS;

            current_stack_depth += 56;
            if (current_stack_depth > max_stack_depth) {
                return VERIFI_ERROR_COMPLEXITY_LIMIT_INSTRUCTIONS;
            }
            current_stack_depth -= 56;
            break;
        }
    }

    return VERIFI_OK;
}

static struct x86_64 _get_x86_64_reg(int vm_reg) {
    static const struct x86_64 mapping[] = {
        {3, 0}, // vm.regs[0] -> RBX
        {1, 0}, // vm.regs[1] -> RCX
        {4, 1}, // vm.regs[2] -> R12
        {5, 1}, // vm.regs[3] -> R13
        {6, 1}, // vm.regs[4] -> R14
        {7, 1}, // vm.regs[5] -> R15
        {3, 1}  // vm.regs[6] -> R11
    };
    return mapping[vm_reg];
}

static unsigned char _make_rex_w(struct x86_64* restrict src, struct x86_64* restrict dst) {
    return 0x48 | (src->is_ext << 2) | dst->is_ext;
}

static int _jit_x86_64_compiler_get_native_instruction_count(int op_code) {
    switch (op_code) {
        case VM_OPC_ADD:                       return 2;
        case VM_OPC_SUB:                       return 2;
        case VM_OPC_MUL:                       return 2;
        case VM_OPC_DIVIDE:                    return 4;
        case VM_OPC_MOV:                       return 1;
        case VM_OPC_MOV_REG:                   return 1;
        case VM_OPC_JMP:                       return 3;
        case VM_OPC_JEQ:
        case VM_OPC_JNE:
        case VM_OPC_JLT:
        case VM_OPC_JLE:
        case VM_OPC_JGT:
        case VM_OPC_JGE:                       return 4;
        case VM_OPC_EXIT:                      return 3;
        case VM_OPC_EXIT_STATUS_CODE:          return 3;
        case VM_OPC_EXEC_FUNCTION:             return 22;
        default:                               return 0;
    }
}

int jit_x86_64_compiler(struct instruction* instr, int instr_count, unsigned char* buffer, int buffer_size) {
    int buff_i = 0;
    int complexity_limit_instr_x86_64 = 0;
    int error_exit_offset = 0;

    int* pc_to_buffer_offset = (int*)kzalloc(instr_count * sizeof(int), GFP_KERNEL);
    if (!pc_to_buffer_offset) {
        return -1;
    }

    // Prologue: save callee‑saved regs, zero all VM registers,
    // initialise complexity counter R10.
    const int bytes_count_prologue = 38;
    if (buff_i + bytes_count_prologue > buffer_size) goto bad;

    buffer[buff_i++] = 0x53;                           // 53: push rbx
    buffer[buff_i++] = 0x41; buffer[buff_i++] = 0x54; // 41 54: push r12
    buffer[buff_i++] = 0x41; buffer[buff_i++] = 0x55; // 41 55: push r13
    buffer[buff_i++] = 0x41; buffer[buff_i++] = 0x56; // 41 56: push r14
    buffer[buff_i++] = 0x41; buffer[buff_i++] = 0x57; // 41 57: push r15

    // Zero all VM registers
    // 31 DB: xor ebx, ebx
    buffer[buff_i++] = 0x31; buffer[buff_i++] = 0xDB;
    // 31 C9: xor ecx, ecx
    buffer[buff_i++] = 0x31; buffer[buff_i++] = 0xC9;
    // 45 31 E4: xor r12d, r12d
    buffer[buff_i++] = 0x45; buffer[buff_i++] = 0x31; buffer[buff_i++] = 0xE4;
    // 45 31 ED: xor r13d, r13d
    buffer[buff_i++] = 0x45; buffer[buff_i++] = 0x31; buffer[buff_i++] = 0xED;
    // 45 31 F6: xor r14d, r14d
    buffer[buff_i++] = 0x45; buffer[buff_i++] = 0x31; buffer[buff_i++] = 0xF6;
    // 45 31 FF: xor r15d, r15d
    buffer[buff_i++] = 0x45; buffer[buff_i++] = 0x31; buffer[buff_i++] = 0xFF;
    // 45 31 DB: xor r11d, r11d
    buffer[buff_i++] = 0x45; buffer[buff_i++] = 0x31; buffer[buff_i++] = 0xFF;

    // 49 BA [imm64]: mov r10, VM_COMPLEXITY_LIMIT_INSTRUCTIONS
    buffer[buff_i++] = 0x49; buffer[buff_i++] = 0xBA;
    *(unsigned long*)(buffer + buff_i) = VM_COMPLEXITY_LIMIT_INSTRUCTIONS;
    buff_i += 8;

    complexity_limit_instr_x86_64 += 13;

    for (int i = 0; i < instr_count; i++) {
        struct instruction curr_instr = instr[i];
        pc_to_buffer_offset[i] = buff_i;

        switch (curr_instr.op_code) {
        case VM_OPC_ADD: {
            const int bytes_count = 6;
            if (buff_i + bytes_count > buffer_size) goto bad;

            struct x86_64 dst  = _get_x86_64_reg(curr_instr.arg0);
            struct x86_64 src1 = _get_x86_64_reg(curr_instr.arg1);
            struct x86_64 src2 = _get_x86_64_reg(curr_instr.arg2);

            // [REX.W] 89 [ModR/M]: mov src1, dst
            buffer[buff_i++] = _make_rex_w(&src1, &dst);
            buffer[buff_i++] = 0x89;
            buffer[buff_i++] = 0xC0 | (src1.code << 3) | dst.code;

            // [REX.W] 01 [ModR/M]: add src2, dst
            buffer[buff_i++] = _make_rex_w(&src2, &dst);
            buffer[buff_i++] = 0x01;
            buffer[buff_i++] = 0xC0 | (src2.code << 3) | dst.code;

            complexity_limit_instr_x86_64 += 2;
            break;
        }
        case VM_OPC_SUB: {
            const int bytes_count = 6;
            if (buff_i + bytes_count > buffer_size) goto bad;

            struct x86_64 dst  = _get_x86_64_reg(curr_instr.arg0);
            struct x86_64 src1 = _get_x86_64_reg(curr_instr.arg1);
            struct x86_64 src2 = _get_x86_64_reg(curr_instr.arg2);

            // [REX.W] 89 [ModR/M]: mov src1, dst
            buffer[buff_i++] = _make_rex_w(&src1, &dst);
            buffer[buff_i++] = 0x89;
            buffer[buff_i++] = 0xC0 | (src1.code << 3) | dst.code;

            // [REX.W] 29 [ModR/M]: sub src2, dst
            buffer[buff_i++] = _make_rex_w(&src2, &dst);
            buffer[buff_i++] = 0x29;
            buffer[buff_i++] = 0xC0 | (src2.code << 3) | dst.code;

            complexity_limit_instr_x86_64 += 2;
            break;
        }
        case VM_OPC_MUL: {
            const int bytes_count = 7;
            if (buff_i + bytes_count > buffer_size) goto bad;

            struct x86_64 dst  = _get_x86_64_reg(curr_instr.arg0);
            struct x86_64 src1 = _get_x86_64_reg(curr_instr.arg1);
            struct x86_64 src2 = _get_x86_64_reg(curr_instr.arg2);

            // [REX.W] 89 [ModR/M]: mov src1, dst
            buffer[buff_i++] = _make_rex_w(&src1, &dst);
            buffer[buff_i++] = 0x89;
            buffer[buff_i++] = 0xC0 | (src1.code << 3) | dst.code;

            // [REX.W] 0F AF [ModR/M]: imul src2, dst
            buffer[buff_i++] = _make_rex_w(&dst, &src2);
            buffer[buff_i++] = 0x0F;
            buffer[buff_i++] = 0xAF;
            buffer[buff_i++] = 0xC0 | (dst.code << 3) | src2.code;

            complexity_limit_instr_x86_64 += 2;
            break;
        }
        case VM_OPC_DIVIDE: {
            const int bytes_count = 11;
            if (buff_i + bytes_count > buffer_size) goto bad;

            struct x86_64 dst  = _get_x86_64_reg(curr_instr.arg0);
            struct x86_64 src1 = _get_x86_64_reg(curr_instr.arg1);
            struct x86_64 src2 = _get_x86_64_reg(curr_instr.arg2);
            struct x86_64 rax_reg = {0, 0}; 

            // [REX.W] 89 [ModR/M]: mov src1, rax
            buffer[buff_i++] = _make_rex_w(&src1, &rax_reg);
            buffer[buff_i++] = 0x89;
            buffer[buff_i++] = 0xC0 | (src1.code << 3) | rax_reg.code;

            // 48 99: cqo (sign-extend rax into rdx:rax)
            buffer[buff_i++] = 0x48;
            buffer[buff_i++] = 0x99;

            // [REX] F7 [ModR/M]: idiv src2
            buffer[buff_i++] = 0x48 | src2.is_ext;
            buffer[buff_i++] = 0xF7;
            buffer[buff_i++] = 0xC0 | (7 << 3) | src2.code;

            // [REX.W] 89 [ModR/M]: mov rax, dst
            buffer[buff_i++] = _make_rex_w(&rax_reg, &dst);
            buffer[buff_i++] = 0x89;
            buffer[buff_i++] = 0xC0 | (rax_reg.code << 3) | dst.code;

            complexity_limit_instr_x86_64 += 4;
            break;
        }
        case VM_OPC_MOV: {
            const int bytes_count = 7;
            if (buff_i + bytes_count > buffer_size) goto bad;

            struct x86_64 dst = _get_x86_64_reg(curr_instr.arg0);

            // [REX] C7 [ModR/M] [imm32]: mov imm32, dst
            buffer[buff_i++] = 0x48 | dst.is_ext; 
            buffer[buff_i++] = 0xC7; 
            buffer[buff_i++] = 0xC0 | dst.code; 

            *(int*)(buffer + buff_i) = curr_instr.imm;
            buff_i += 4;

            complexity_limit_instr_x86_64 += 1;
            break;
        }
        case VM_OPC_MOV_REG: {
            const int bytes_count = 3;
            if (buff_i + bytes_count > buffer_size) goto bad;

            struct x86_64 dst  = _get_x86_64_reg(curr_instr.arg0);
            struct x86_64 src1 = _get_x86_64_reg(curr_instr.arg1);

            // [REX.W] 89 [ModR/M]: mov src1, dst
            buffer[buff_i++] = _make_rex_w(&src1, &dst);
            buffer[buff_i++] = 0x89;
            buffer[buff_i++] = 0xC0 | (src1.code << 3) | dst.code;

            complexity_limit_instr_x86_64 += 1;
            break;
        }
        case VM_OPC_JMP: {
            const int bytes_count = 18;
            if (buff_i + bytes_count > buffer_size) goto bad;

            int native_cost = 0;
            if (curr_instr.imm < 0) {
                int target_pc = i + curr_instr.imm;
                for (int j = target_pc; j <= i; j++) {
                    native_cost += _jit_x86_64_compiler_get_native_instruction_count(instr[j].op_code);
                }
            } else {
                native_cost = _jit_x86_64_compiler_get_native_instruction_count(VM_OPC_JMP);
            }

            // 49 81 EA [imm32]: sub imm32, r10 (decrement complexity counter)
            buffer[buff_i++] = 0x49;
            buffer[buff_i++] = 0x81;
            buffer[buff_i++] = 0xEA;
            *(int*)(buffer + buff_i) = native_cost;
            buff_i += 4;

            // 0F 8E [rel32]: jle rel32 (jump to error exit epilogue if r10 <= 0)
            buffer[buff_i++] = 0x0F; 
            buffer[buff_i++] = 0x8E;
            *(int*)(buffer + buff_i) = 0; 
            buff_i += 4;

            // E9 [rel32]: jmp rel32 (unconditional relative jump to target PC)
            buffer[buff_i++] = 0xE9; 
            *(int*)(buffer + buff_i) = 0;
            buff_i += 4;

            complexity_limit_instr_x86_64 += 3;
            break;
        }
        case VM_OPC_JEQ:
        case VM_OPC_JNE:
        case VM_OPC_JLT:
        case VM_OPC_JLE:
        case VM_OPC_JGT:
        case VM_OPC_JGE: {
            const int bytes_count = 22;
            if (buff_i + bytes_count > buffer_size) goto bad;

            int native_cost = 0;
            if (curr_instr.imm < 0) {
                int target_pc = i + curr_instr.imm;
                for (int j = target_pc; j <= i; j++) {
                    native_cost += _jit_x86_64_compiler_get_native_instruction_count(instr[j].op_code);
                }
            } else {
                native_cost = _jit_x86_64_compiler_get_native_instruction_count(curr_instr.op_code);
            }

            // 49 81 EA [imm32]: sub imm32, r10 (decrement complexity counter)
            buffer[buff_i++] = 0x49;
            buffer[buff_i++] = 0x81;
            buffer[buff_i++] = 0xEA;
            *(int*)(buffer + buff_i) = native_cost;
            buff_i += 4;

            // 0F 8E [rel32]: jle rel32 (jump to error exit epilogue if r10 <= 0)
            buffer[buff_i++] = 0x0F; 
            buffer[buff_i++] = 0x8E;
            *(int*)(buffer + buff_i) = 0; 
            buff_i += 4;

            struct x86_64 reg0 = _get_x86_64_reg(curr_instr.arg0);
            struct x86_64 reg1 = _get_x86_64_reg(curr_instr.arg1);
            
            // [REX.W] 39 [ModR/M]: cmp reg1, reg0
            buffer[buff_i++] = _make_rex_w(&reg1, &reg0);
            buffer[buff_i++] = 0x39;
            buffer[buff_i++] = 0xC0 | (reg1.code << 3) | reg0.code;

            // 0F 8x [rel32]: jcc rel32 (conditional relative jump based on flag)
            buffer[buff_i++] = 0x0F;
            if (curr_instr.op_code == VM_OPC_JEQ)       buffer[buff_i++] = 0x84; // 0F 84: jz rel32
            else if (curr_instr.op_code == VM_OPC_JNE)  buffer[buff_i++] = 0x85; // 0F 85: jnz rel32
            else if (curr_instr.op_code == VM_OPC_JLT)  buffer[buff_i++] = 0x8C; // 0F 8C: jl rel32
            else if (curr_instr.op_code == VM_OPC_JLE)  buffer[buff_i++] = 0x8E; // 0F 8E: jle rel32
            else if (curr_instr.op_code == VM_OPC_JGT)  buffer[buff_i++] = 0x8F; // 0F 8F: jg rel32
            else if (curr_instr.op_code == VM_OPC_JGE)  buffer[buff_i++] = 0x8D; // 0F 8D: jge rel32

            *(int*)(buffer + buff_i) = 0;
            buff_i += 4;

            complexity_limit_instr_x86_64 += 4;
            break;
        }
        case VM_OPC_EXIT: {
            const int bytes_count = 15;
            if (buff_i + bytes_count > buffer_size) goto bad;

            struct x86_64 src = _get_x86_64_reg(curr_instr.arg0);
            struct x86_64 rax_reg = {0, 0}; 

            // [REX.W] 89 [ModR/M]: mov src, rax (set return value)
            buffer[buff_i++] = _make_rex_w(&src, &rax_reg); 
            buffer[buff_i++] = 0x89;
            buffer[buff_i++] = 0xC0 | (src.code << 3) | rax_reg.code;

            // Epilogue: restore callee‑saved registers and return
            buffer[buff_i++] = 0x41; buffer[buff_i++] = 0x5F; // 41 5F: pop r15
            buffer[buff_i++] = 0x41; buffer[buff_i++] = 0x5E; // 41 5E: pop r14
            buffer[buff_i++] = 0x41; buffer[buff_i++] = 0x5D; // 41 5D: pop r13
            buffer[buff_i++] = 0x41; buffer[buff_i++] = 0x5C; // 41 5C: pop r12
            buffer[buff_i++] = 0x5B;                           // 5B: pop rbx
            buffer[buff_i++] = 0xC3;                           // C3: ret

            complexity_limit_instr_x86_64 += 3;
            break;
        }
        case VM_OPC_EXIT_STATUS_CODE: {
            const int bytes_count = 17;
            if (buff_i + bytes_count > buffer_size) goto bad;

            if (curr_instr.imm == 0) {
                // 48 31 C0: xor rax, rax
                buffer[buff_i++] = 0x48;
                buffer[buff_i++] = 0x31;
                buffer[buff_i++] = 0xC0;
            } else {
                // 48 C7 C0 [imm32]: mov imm32, rax
                buffer[buff_i++] = 0x48;
                buffer[buff_i++] = 0xC7;
                buffer[buff_i++] = 0xC0;
                *(int*)(buffer + buff_i) = curr_instr.imm;
                buff_i += 4;
            }

            // Epilogue: restore callee‑saved registers and return
            buffer[buff_i++] = 0x41; buffer[buff_i++] = 0x5F; // 41 5F: pop r15
            buffer[buff_i++] = 0x41; buffer[buff_i++] = 0x5E; // 41 5E: pop r14
            buffer[buff_i++] = 0x41; buffer[buff_i++] = 0x5D; // 41 5D: pop r13
            buffer[buff_i++] = 0x41; buffer[buff_i++] = 0x5C; // 41 5C: pop r12
            buffer[buff_i++] = 0x5B;                           // 5B: pop rbx
            buffer[buff_i++] = 0xC3;                           // C3: ret

            complexity_limit_instr_x86_64 += 3;
            break;
        }
        case VM_OPC_EXEC_FUNCTION: {
            struct x86_64 abi_regs[] = {
                {7, 0}, // RDI
                {6, 0}, // RSI
                {2, 0}, // RDX
                {1, 0}, // RCX
                {0, 1}, // R8
                {1, 1}  // R9
            };
            int abi_regs_size = sizeof(abi_regs) / sizeof(abi_regs[0]);

            const int bytes_count = 75 + 7 + 7; 
            if (buff_i + bytes_count > buffer_size) goto bad;

            // 48 83 EC 08: sub 0x8, rsp (align stack to 16 bytes boundary)
            buffer[buff_i++] = 0x48; 
            buffer[buff_i++] = 0x83; 
            buffer[buff_i++] = 0xEC; 
            buffer[buff_i++] = 0x08;

            // Preserve VM context before calling native helper
            buffer[buff_i++] = 0x53;                           // 53: push rbx
            buffer[buff_i++] = 0x41; buffer[buff_i++] = 0x54; // 41 54: push r12
            buffer[buff_i++] = 0x41; buffer[buff_i++] = 0x55; // 41 55: push r13
            buffer[buff_i++] = 0x41; buffer[buff_i++] = 0x56; // 41 56: push r14
            buffer[buff_i++] = 0x41; buffer[buff_i++] = 0x57; // 41 57: push r15
            buffer[buff_i++] = 0x41; buffer[buff_i++] = 0x52; // 41 52: push r10
            buffer[buff_i++] = 0x41; buffer[buff_i++] = 0x53; // 41 53: push r11

            // Load function arguments from VM registers into System V ABI layout
            for (int reg_idx = 0; reg_idx < abi_regs_size; reg_idx++) {
                struct x86_64 src = _get_x86_64_reg(reg_idx);
                struct x86_64 dst = abi_regs[reg_idx];
                // [REX.W] 89 [ModR/M]: mov src, dst
                buffer[buff_i++] = _make_rex_w(&src, &dst);
                buffer[buff_i++] = 0x89;
                buffer[buff_i++] = 0xC0 | (src.code << 3) | dst.code;
            }

            // 48 B8 [imm64]: mov imm64, rax (load target function absolute address)
            buffer[buff_i++] = 0x48; buffer[buff_i++] = 0xB8; 
            *(unsigned long*)(buffer + buff_i) = (unsigned long)vm_functions[curr_instr.imm];
            buff_i += 8;
            
            // FF D0: call rax
            buffer[buff_i++] = 0xFF; buffer[buff_i++] = 0xD0;

            // Restore VM context state
            buffer[buff_i++] = 0x41; buffer[buff_i++] = 0x5B; // 41 5B: pop r11
            buffer[buff_i++] = 0x41; buffer[buff_i++] = 0x5A; // 41 5A: pop r10
            buffer[buff_i++] = 0x41; buffer[buff_i++] = 0x5F; // 41 5F: pop r15
            buffer[buff_i++] = 0x41; buffer[buff_i++] = 0x5E; // 41 5E: pop r14
            buffer[buff_i++] = 0x41; buffer[buff_i++] = 0x5D; // 41 5D: pop r13
            buffer[buff_i++] = 0x41; buffer[buff_i++] = 0x5C; // 41 5C: pop r12
            buffer[buff_i++] = 0x5B;                           // 5B: pop rbx

            // 48 83 C4 08: add 0x8, rsp (restore stack pointer after alignment)
            buffer[buff_i++] = 0x48; 
            buffer[buff_i++] = 0x83; 
            buffer[buff_i++] = 0xC4; 
            buffer[buff_i++] = 0x08;

            // Move function result to the VM destination register
            struct x86_64 target_reg = _get_x86_64_reg(curr_instr.arg0);
            struct x86_64 rax_reg = {0, 0};
            // [REX.W] 89 [ModR/M]: mov rax, target_reg
            buffer[buff_i++] = _make_rex_w(&rax_reg, &target_reg);
            buffer[buff_i++] = 0x89;
            buffer[buff_i++] = 0xC0 | (rax_reg.code << 3) | target_reg.code;

            complexity_limit_instr_x86_64 += 24;
            break;
        }
        default:
            goto bad;
        }
    }

    // Epilogue – error exit path (complexity limit hit)
    const int bytes_count_epilogue = 19;
    if (buff_i + bytes_count_epilogue > buffer_size) goto bad;

    error_exit_offset = buff_i;

    // 48 C7 C0 [imm32]: mov VM_EXEC_ERROR_LIMIT_INSTRUCTIONS, rax (set failure code)
    buffer[buff_i++] = 0x48; buffer[buff_i++] = 0xC7; buffer[buff_i++] = 0xC0;
    *(int*)(buffer + buff_i) = VM_EXEC_ERROR_LIMIT_INSTRUCTIONS; buff_i += 4;

    buffer[buff_i++] = 0x41; buffer[buff_i++] = 0x5F; // 41 5F: pop r15
    buffer[buff_i++] = 0x41; buffer[buff_i++] = 0x5E; // 41 5E: pop r14
    buffer[buff_i++] = 0x41; buffer[buff_i++] = 0x5D; // 41 5D: pop r13
    buffer[buff_i++] = 0x41; buffer[buff_i++] = 0x5C; // 41 5C: pop r12
    buffer[buff_i++] = 0x5B;                           // 5B: pop rbx
    buffer[buff_i++] = 0xC3;                           // C3: ret

    // Backpatch jump offsets
    for (int i = 0; i < instr_count; i++) {
        struct instruction curr_instr = instr[i];
        int current_instr_bytes_start = pc_to_buffer_offset[i];

        if (curr_instr.op_code == VM_OPC_JMP) {
            int jz_next_instr_offset = current_instr_bytes_start + 13;
            *(int*)(buffer + current_instr_bytes_start + 9) = error_exit_offset - jz_next_instr_offset;

            int target_vm_pc = i + curr_instr.imm; 
            int target_bytes_offset = pc_to_buffer_offset[target_vm_pc];
            int jmp_next_instr_offset = current_instr_bytes_start + 18;
            *(int*)(buffer + current_instr_bytes_start + 14) = target_bytes_offset - jmp_next_instr_offset;
        } else if (curr_instr.op_code >= VM_OPC_JEQ && curr_instr.op_code <= VM_OPC_JGE) {
            int jz_next_instr_offset = current_instr_bytes_start + 13;
            *(int*)(buffer + current_instr_bytes_start + 9) = error_exit_offset - jz_next_instr_offset;

            int target_vm_pc = i + curr_instr.imm;
            int target_bytes_offset = pc_to_buffer_offset[target_vm_pc];
            int jcc_next_instr_offset = current_instr_bytes_start + 22;
            *(int*)(buffer + current_instr_bytes_start + 18) = target_bytes_offset - jcc_next_instr_offset;
        }
    }

    if (complexity_limit_instr_x86_64 > VM_COMPLEXITY_LIMIT_INSTRUCTIONS) {
        goto bad;
    }

    kfree(pc_to_buffer_offset);
    return buff_i;

bad:
    kfree(pc_to_buffer_offset);
    return -1;
}