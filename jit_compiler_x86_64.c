#include "jit_compiler_x86_64.h"
#include "vm_functions.h"

#include <linux/kernel.h>
#include <linux/slab.h>

struct x86_64 {
    int code;
    int is_ext;
};

struct jit_ctx {
    unsigned char* buffer;
    int buffer_size;
    int pos;
    int complexity_limit_instr_x86_64;
    int error_exit_offset;
    int div_zero_exit_offset;
};

static int _check_register_num(unsigned char arg) {
    if (arg >= MAX_REGS) {
        return 0;
    }
    return 1;
}

static int _check_registers_values(
    unsigned char arg0, unsigned char arg1, unsigned char arg2, int imm,
    int arg0_set, int arg1_set, int arg2_set, int imm_set
) {
    if (!arg0_set && arg0 != 0) return 0;
    if (!arg1_set && arg1 != 0) return 0;
    if (!arg2_set && arg2 != 0) return 0;
    if (!imm_set && imm != 0) return 0;
    return 1;
}

int verificator(struct instruction* instr, int instr_count) {
    for (int i = 0; i < instr_count; i++) {
        struct instruction curr_instr = instr[i];

        if (curr_instr.op_code > VM_OPC_LOAD_STACK || curr_instr.op_code == 0) {
            return VERIFI_ERROR_INCORRECT_OP_CODE;
        }

        if (!_check_register_num(curr_instr.arg0)
            || !_check_register_num(curr_instr.arg1)
            || !_check_register_num(curr_instr.arg2)) return VERIFI_ERROR_REGISTER;

        if (curr_instr.arg0 == VM_REGSP) {
            if (curr_instr.op_code == VM_OPC_ADD ||
                curr_instr.op_code == VM_OPC_SUB ||
                curr_instr.op_code == VM_OPC_MUL ||
                curr_instr.op_code == VM_OPC_DIVIDE ||
                curr_instr.op_code == VM_OPC_MOV ||
                curr_instr.op_code == VM_OPC_MOV_REG ||
                curr_instr.op_code == VM_OPC_LOAD_STACK ||
                curr_instr.op_code == VM_OPC_EXEC_FUNCTION) {
                
                return VERIFI_ERROR_READ_ONLY_REGISTER;
            }
        }

        switch (curr_instr.op_code) {
        case VM_OPC_ADD:
        case VM_OPC_SUB:
        case VM_OPC_MUL:
        case VM_OPC_DIVIDE:
            if (!_check_registers_values(curr_instr.arg0, curr_instr.arg1, curr_instr.arg2, curr_instr.imm, 1, 1, 1, 0)) 
                return VERIFI_ERROR_REGISTERS_VALUES;
            break;
            
        case VM_OPC_MOV:
            if (!_check_registers_values(curr_instr.arg0, curr_instr.arg1, curr_instr.arg2, curr_instr.imm, 1, 0, 0, 1)) 
                return VERIFI_ERROR_REGISTERS_VALUES;
            break;
            
        case VM_OPC_MOV_REG:
            if (!_check_registers_values(curr_instr.arg0, curr_instr.arg1, curr_instr.arg2, curr_instr.imm, 1, 1, 0, 0)) 
                return VERIFI_ERROR_REGISTERS_VALUES;
            break;

        case VM_OPC_STORE_STACK:
        case VM_OPC_LOAD_STACK:
            if (!_check_registers_values(curr_instr.arg0, curr_instr.arg1, curr_instr.arg2, curr_instr.imm, 1, 0, 0, 1)) 
                return VERIFI_ERROR_REGISTERS_VALUES;
            

            if (curr_instr.imm % 8 != 0) {
                return VERIFI_ERROR_STACK_ALIGNMENT;
            }
            
            if (curr_instr.imm < 0 || curr_instr.imm > 504) {
                return VERIFI_ERROR_STACK_OUT_OF_BOUNDS;
            }
            break;

        case VM_OPC_JMP:
        case VM_OPC_JEQ:
        case VM_OPC_JNE:
        case VM_OPC_JLT:
        case VM_OPC_JLE:
        case VM_OPC_JGT:
        case VM_OPC_JGE: {
            int is_jmp = (curr_instr.op_code == VM_OPC_JMP);
            if (!_check_registers_values(curr_instr.arg0, curr_instr.arg1, curr_instr.arg2, curr_instr.imm, !is_jmp, !is_jmp, 0, 1)) 
                return VERIFI_ERROR_REGISTERS_VALUES;

            int offset = curr_instr.imm;
            if (offset == 0) return VERIFI_ERROR_INFINITE_LOOP; 

            int target_pc = i + offset;
            if (target_pc < 0 || target_pc >= instr_count) return VERIFI_ERROR_OUT_OF_PROGRAMM;
            break;
        }
        case VM_OPC_EXIT:
            if (!_check_registers_values(curr_instr.arg0, curr_instr.arg1, curr_instr.arg2, curr_instr.imm, 1, 0, 0, 0)) 
                return VERIFI_ERROR_REGISTERS_VALUES;
            break;
            
        case VM_OPC_EXIT_STATUS_CODE:
            if (!_check_registers_values(curr_instr.arg0, curr_instr.arg1, curr_instr.arg2, curr_instr.imm, 0, 0, 0, 1)) 
                return VERIFI_ERROR_REGISTERS_VALUES;
            break;
            
        case VM_OPC_EXEC_FUNCTION:
            if (!_check_registers_values(curr_instr.arg0, curr_instr.arg1, curr_instr.arg2, curr_instr.imm, 1, 0, 0, 1)) 
                return VERIFI_ERROR_REGISTERS_VALUES;

            if (curr_instr.imm < 0 || (unsigned)curr_instr.imm >= VM_FUNCTIONS_COUNT)
                return VERIFI_ERROR_FUNCTION_DOESNT_EXISTS;
            break;
        }
    }

    return VERIFI_OK;
}

static struct x86_64 _get_x86_64_reg(int vm_reg) {
    static const struct x86_64 mapping[] = {
        {3, 0}, // vm.regs[0] -> RBX
        {4, 1}, // vm.regs[1] -> R12
        {5, 1}, // vm.regs[2] -> R13
        {6, 1}, // vm.regs[3] -> R14
        {7, 1}, // vm.regs[4] -> R15
        {2, 1}, // vm.regs[5] -> R10
        {3, 1}, // vm.regs[6] -> R11
        {5, 0}  // vm.regs[7] -> RBP
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
        case VM_OPC_DIVIDE:                    return 6;
        case VM_OPC_MOV:                       return 1;
        case VM_OPC_MOV_REG:                   return 1;
        case VM_OPC_STORE_STACK:               return 1;
        case VM_OPC_LOAD_STACK:                return 1;
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

static inline int check_space(struct jit_ctx* ctx, int required) {
    return (ctx->pos + required <= ctx->buffer_size);
}

static int emit_add_sub(struct jit_ctx* ctx, struct instruction* instr, int is_sub) {
    if (!check_space(ctx, 6)) return -1;
    struct x86_64 dst = _get_x86_64_reg(instr->arg0);
    struct x86_64 src1 = _get_x86_64_reg(instr->arg1);
    struct x86_64 src2 = _get_x86_64_reg(instr->arg2);

    ctx->buffer[ctx->pos++] = _make_rex_w(&src1, &dst);
    ctx->buffer[ctx->pos++] = 0x89;
    ctx->buffer[ctx->pos++] = 0xC0 | (src1.code << 3) | dst.code;

    ctx->buffer[ctx->pos++] = _make_rex_w(&src2, &dst);
    ctx->buffer[ctx->pos++] = is_sub ? 0x29 : 0x01;
    ctx->buffer[ctx->pos++] = 0xC0 | (src2.code << 3) | dst.code;

    ctx->complexity_limit_instr_x86_64 += 2;
    return 0;
}

static int emit_mul(struct jit_ctx* ctx, struct instruction* instr) {
    if (!check_space(ctx, 7)) return -1;
    struct x86_64 dst = _get_x86_64_reg(instr->arg0);
    struct x86_64 src1 = _get_x86_64_reg(instr->arg1);
    struct x86_64 src2 = _get_x86_64_reg(instr->arg2);

    ctx->buffer[ctx->pos++] = _make_rex_w(&src1, &dst);
    ctx->buffer[ctx->pos++] = 0x89;
    ctx->buffer[ctx->pos++] = 0xC0 | (src1.code << 3) | dst.code;

    ctx->buffer[ctx->pos++] = _make_rex_w(&dst, &src2);
    ctx->buffer[ctx->pos++] = 0x0F; ctx->buffer[ctx->pos++] = 0xAF;
    ctx->buffer[ctx->pos++] = 0xC0 | (dst.code << 3) | src2.code;

    ctx->complexity_limit_instr_x86_64 += 2;
    return 0;
}

static int emit_divide(struct jit_ctx* ctx, struct instruction* instr, int instruction_index, int* div_fixups, int* div_fixup_count) {
    if (!check_space(ctx, 20)) return -1;
    struct x86_64 dst = _get_x86_64_reg(instr->arg0);
    struct x86_64 src1 = _get_x86_64_reg(instr->arg1);
    struct x86_64 src2 = _get_x86_64_reg(instr->arg2);
    struct x86_64 rax_reg = {0, 0}; 

    ctx->buffer[ctx->pos++] = 0x48 | src2.is_ext;
    ctx->buffer[ctx->pos++] = 0x85;
    ctx->buffer[ctx->pos++] = 0xC0 | (src2.code << 3) | src2.code;

    ctx->buffer[ctx->pos++] = 0x0F; ctx->buffer[ctx->pos++] = 0x84;
    div_fixups[(*div_fixup_count)++] = ctx->pos;
    ctx->pos += 4;

    ctx->buffer[ctx->pos++] = _make_rex_w(&src1, &rax_reg);
    ctx->buffer[ctx->pos++] = 0x89;
    ctx->buffer[ctx->pos++] = 0xC0 | (src1.code << 3) | rax_reg.code;

    ctx->buffer[ctx->pos++] = 0x48; ctx->buffer[ctx->pos++] = 0x99;

    ctx->buffer[ctx->pos++] = 0x48 | src2.is_ext;
    ctx->buffer[ctx->pos++] = 0xF7;
    ctx->buffer[ctx->pos++] = 0xC0 | (7 << 3) | src2.code;

    ctx->buffer[ctx->pos++] = _make_rex_w(&rax_reg, &dst);
    ctx->buffer[ctx->pos++] = 0x89;
    ctx->buffer[ctx->pos++] = 0xC0 | (rax_reg.code << 3) | dst.code;

    ctx->complexity_limit_instr_x86_64 += 6;
    return 0;
}

static int emit_mov(struct jit_ctx* ctx, struct instruction* instr) {
    if (!check_space(ctx, 7)) return -1;
    struct x86_64 dst = _get_x86_64_reg(instr->arg0);
    ctx->buffer[ctx->pos++] = 0x48 | dst.is_ext; 
    ctx->buffer[ctx->pos++] = 0xC7; 
    ctx->buffer[ctx->pos++] = 0xC0 | dst.code; 
    *(int*)(ctx->buffer + ctx->pos) = instr->imm; ctx->pos += 4;
    ctx->complexity_limit_instr_x86_64 += 1;
    return 0;
}

static int emit_mov_reg(struct jit_ctx* ctx, struct instruction* instr) {
    if (!check_space(ctx, 3)) return -1;
    struct x86_64 dst = _get_x86_64_reg(instr->arg0);
    struct x86_64 src = _get_x86_64_reg(instr->arg1);
    ctx->buffer[ctx->pos++] = _make_rex_w(&src, &dst);
    ctx->buffer[ctx->pos++] = 0x89;
    ctx->buffer[ctx->pos++] = 0xC0 | (src.code << 3) | dst.code;
    ctx->complexity_limit_instr_x86_64 += 1;
    return 0;
}

static int emit_store_stack(struct jit_ctx* ctx, struct instruction* instr) {
    if (!check_space(ctx, 7)) return -1;
    struct x86_64 src = _get_x86_64_reg(instr->arg0);
    
    ctx->buffer[ctx->pos++] = 0x48 | (src.is_ext << 2);
    ctx->buffer[ctx->pos++] = 0x89;
    ctx->buffer[ctx->pos++] = 0x80 | (src.code << 3) | 5; 
    
    *(int*)(ctx->buffer + ctx->pos) = instr->imm; 
    ctx->pos += 4;
    
    ctx->complexity_limit_instr_x86_64 += 1;
    return 0;
}

static int emit_load_stack(struct jit_ctx* ctx, struct instruction* instr) {
    if (!check_space(ctx, 7)) return -1;
    struct x86_64 dst = _get_x86_64_reg(instr->arg0);
    
    ctx->buffer[ctx->pos++] = 0x48 | (dst.is_ext << 2);
    ctx->buffer[ctx->pos++] = 0x8B;
    ctx->buffer[ctx->pos++] = 0x80 | (dst.code << 3) | 5; 
    
    *(int*)(ctx->buffer + ctx->pos) = instr->imm; 
    ctx->pos += 4;
    
    ctx->complexity_limit_instr_x86_64 += 1;
    return 0;
}

static int emit_exec_function(struct jit_ctx* ctx, struct instruction* instr) {
    struct x86_64 abi_regs[] = { {7, 0}, {6, 0}, {2, 0}, {1, 0}, {0, 1}, {1, 1} };
    int abi_regs_size = sizeof(abi_regs) / sizeof(abi_regs[0]);

    if (!check_space(ctx, 90)) return -1;
    
    ctx->buffer[ctx->pos++] = 0x55;                           // push rbp
    ctx->buffer[ctx->pos++] = 0x53;                           // push rbx
    ctx->buffer[ctx->pos++] = 0x41; ctx->buffer[ctx->pos++] = 0x54; // push r12
    ctx->buffer[ctx->pos++] = 0x41; ctx->buffer[ctx->pos++] = 0x55; // push r13
    ctx->buffer[ctx->pos++] = 0x41; ctx->buffer[ctx->pos++] = 0x56; // push r14
    ctx->buffer[ctx->pos++] = 0x41; ctx->buffer[ctx->pos++] = 0x57; // push r15
    ctx->buffer[ctx->pos++] = 0x41; ctx->buffer[ctx->pos++] = 0x52; // push r10
    ctx->buffer[ctx->pos++] = 0x41; ctx->buffer[ctx->pos++] = 0x53; // push r11

    for (int reg_idx = 0; reg_idx < abi_regs_size; reg_idx++) {
        struct x86_64 src = _get_x86_64_reg(reg_idx);
        struct x86_64 dst = abi_regs[reg_idx];
        ctx->buffer[ctx->pos++] = _make_rex_w(&src, &dst);
        ctx->buffer[ctx->pos++] = 0x89;
        ctx->buffer[ctx->pos++] = 0xC0 | (src.code << 3) | dst.code;
    }

    ctx->buffer[ctx->pos++] = 0x48; ctx->buffer[ctx->pos++] = 0xB8; 
    *(unsigned long*)(ctx->buffer + ctx->pos) = (unsigned long)vm_functions[instr->imm];
    ctx->pos += 8;
    
    ctx->buffer[ctx->pos++] = 0xFF; ctx->buffer[ctx->pos++] = 0xD0; // call rax

    ctx->buffer[ctx->pos++] = 0x41; ctx->buffer[ctx->pos++] = 0x5B; // pop r11
    ctx->buffer[ctx->pos++] = 0x41; ctx->buffer[ctx->pos++] = 0x5A; // pop r10
    ctx->buffer[ctx->pos++] = 0x41; ctx->buffer[ctx->pos++] = 0x5F; // pop r15
    ctx->buffer[ctx->pos++] = 0x41; ctx->buffer[ctx->pos++] = 0x5E; // pop r14
    ctx->buffer[ctx->pos++] = 0x41; ctx->buffer[ctx->pos++] = 0x5D; // pop r13
    ctx->buffer[ctx->pos++] = 0x41; ctx->buffer[ctx->pos++] = 0x5C; // pop r12
    ctx->buffer[ctx->pos++] = 0x5B;                                 // pop rbx
    ctx->buffer[ctx->pos++] = 0x5D;                                 // pop rbp

    struct x86_64 target_reg = _get_x86_64_reg(instr->arg0);
    struct x86_64 rax_reg = {0, 0};
    ctx->buffer[ctx->pos++] = _make_rex_w(&rax_reg, &target_reg);
    ctx->buffer[ctx->pos++] = 0x89;
    ctx->buffer[ctx->pos++] = 0xC0 | (rax_reg.code << 3) | target_reg.code;

    ctx->complexity_limit_instr_x86_64 += 24;
    return 0;
}

int jit_x86_64_compiler(struct instruction* instr, int instr_count, unsigned char* buffer, int buffer_size) {
    struct jit_ctx ctx = { .buffer = buffer, .buffer_size = buffer_size, .pos = 0, .complexity_limit_instr_x86_64 = 0 };

    int* pc_to_buffer_offset = (int*)kzalloc(instr_count * sizeof(int), GFP_KERNEL);
    int* div_fixups = (int*)kzalloc(instr_count * sizeof(int), GFP_KERNEL);
    int div_fixup_count = 0;

    if (!pc_to_buffer_offset || !div_fixups) goto bad;

    // Prologue
    if (!check_space(&ctx, 65)) goto bad;
    ctx.buffer[ctx.pos++] = 0x55;                               // push rbp
    ctx.buffer[ctx.pos++] = 0x53;                               // push rbx
    ctx.buffer[ctx.pos++] = 0x41; ctx.buffer[ctx.pos++] = 0x54; // push r12
    ctx.buffer[ctx.pos++] = 0x41; ctx.buffer[ctx.pos++] = 0x55; // push r13
    ctx.buffer[ctx.pos++] = 0x41; ctx.buffer[ctx.pos++] = 0x56; // push r14
    ctx.buffer[ctx.pos++] = 0x41; ctx.buffer[ctx.pos++] = 0x57; // push r15

    // sub rsp, 520
    ctx.buffer[ctx.pos++] = 0x48; ctx.buffer[ctx.pos++] = 0x81; ctx.buffer[ctx.pos++] = 0xEC;
    *(int*)(ctx.buffer + ctx.pos) = 520; ctx.pos += 4;
    
    // mov rbp, rsp
    ctx.buffer[ctx.pos++] = 0x48; ctx.buffer[ctx.pos++] = 0x89; ctx.buffer[ctx.pos++] = 0xE5;

    // Zero VM registers according to new mapping
    ctx.buffer[ctx.pos++] = 0x31; ctx.buffer[ctx.pos++] = 0xDB; // rbx (regs[0])
    ctx.buffer[ctx.pos++] = 0x45; ctx.buffer[ctx.pos++] = 0x31; ctx.buffer[ctx.pos++] = 0xE4; // r12 (regs[1])
    ctx.buffer[ctx.pos++] = 0x45; ctx.buffer[ctx.pos++] = 0x31; ctx.buffer[ctx.pos++] = 0xED; // r13 (regs[2])
    ctx.buffer[ctx.pos++] = 0x45; ctx.buffer[ctx.pos++] = 0x31; ctx.buffer[ctx.pos++] = 0xF6; // r14 (regs[3])
    ctx.buffer[ctx.pos++] = 0x45; ctx.buffer[ctx.pos++] = 0x31; ctx.buffer[ctx.pos++] = 0xFF; // r15 (regs[4])
    ctx.buffer[ctx.pos++] = 0x45; ctx.buffer[ctx.pos++] = 0x31; ctx.buffer[ctx.pos++] = 0xD2; // r10 (regs[5])
    ctx.buffer[ctx.pos++] = 0x45; ctx.buffer[ctx.pos++] = 0x31; ctx.buffer[ctx.pos++] = 0xD9; // r11 (regs[6])

    // mov r10, VM_COMPLEXITY_LIMIT
    ctx.buffer[ctx.pos++] = 0x49; ctx.buffer[ctx.pos++] = 0xBA;
    *(unsigned long*)(ctx.buffer + ctx.pos) = VM_COMPLEXITY_LIMIT_INSTRUCTIONS; ctx.pos += 8;
    ctx.complexity_limit_instr_x86_64 += 13;

    for (int i = 0; i < instr_count; i++) {
        struct instruction curr_instr = instr[i];
        pc_to_buffer_offset[i] = ctx.pos;

        switch (curr_instr.op_code) {
        case VM_OPC_ADD: if (emit_add_sub(&ctx, &curr_instr, 0) < 0) goto bad; break;
        case VM_OPC_SUB: if (emit_add_sub(&ctx, &curr_instr, 1) < 0) goto bad; break;
        case VM_OPC_MUL: if (emit_mul(&ctx, &curr_instr) < 0) goto bad; break;
        case VM_OPC_DIVIDE: if (emit_divide(&ctx, &curr_instr, i, div_fixups, &div_fixup_count) < 0) goto bad; break;
        case VM_OPC_MOV: if (emit_mov(&ctx, &curr_instr) < 0) goto bad; break;
        case VM_OPC_MOV_REG: if (emit_mov_reg(&ctx, &curr_instr) < 0) goto bad; break;
        case VM_OPC_STORE_STACK: if (emit_store_stack(&ctx, &curr_instr) < 0) goto bad; break;
        case VM_OPC_LOAD_STACK: if (emit_load_stack(&ctx, &curr_instr) < 0) goto bad; break;
        
        case VM_OPC_JMP:
        case VM_OPC_JEQ:
        case VM_OPC_JNE:
        case VM_OPC_JLT:
        case VM_OPC_JLE:
        case VM_OPC_JGT:
        case VM_OPC_JGE: {
            int is_jmp = (curr_instr.op_code == VM_OPC_JMP);
            if (!check_space(&ctx, is_jmp ? 18 : 22)) goto bad;
            int native_cost = 0;
            if (curr_instr.imm < 0) {
                for (int j = i + curr_instr.imm; j <= i; j++) 
                    native_cost += _jit_x86_64_compiler_get_native_instruction_count(instr[j].op_code);
            } else {
                native_cost = _jit_x86_64_compiler_get_native_instruction_count(curr_instr.op_code);
            }

            // sub r10, native_cost
            ctx.buffer[ctx.pos++] = 0x49; ctx.buffer[ctx.pos++] = 0x81; ctx.buffer[ctx.pos++] = 0xEA;
            *(int*)(ctx.buffer + ctx.pos) = native_cost; ctx.pos += 4;

            // jle error_exit
            ctx.buffer[ctx.pos++] = 0x0F; ctx.buffer[ctx.pos++] = 0x8E; 
            *(int*)(ctx.buffer + ctx.pos) = 0; ctx.pos += 4;

            if (is_jmp) {
                ctx.buffer[ctx.pos++] = 0xE9;
                *(int*)(ctx.buffer + ctx.pos) = 0; ctx.pos += 4;
            } else {
                struct x86_64 reg0 = _get_x86_64_reg(curr_instr.arg0);
                struct x86_64 reg1 = _get_x86_64_reg(curr_instr.arg1);
                ctx.buffer[ctx.pos++] = _make_rex_w(&reg1, &reg0);
                ctx.buffer[ctx.pos++] = 0x39; 
                ctx.buffer[ctx.pos++] = 0xC0 | (reg1.code << 3) | reg0.code;

                ctx.buffer[ctx.pos++] = 0x0F;
                if (curr_instr.op_code == VM_OPC_JEQ) ctx.buffer[ctx.pos++] = 0x84;
                else if (curr_instr.op_code == VM_OPC_JNE) ctx.buffer[ctx.pos++] = 0x85;
                else if (curr_instr.op_code == VM_OPC_JLT) ctx.buffer[ctx.pos++] = 0x8C;
                else if (curr_instr.op_code == VM_OPC_JLE) ctx.buffer[ctx.pos++] = 0x8E;
                else if (curr_instr.op_code == VM_OPC_JGT) ctx.buffer[ctx.pos++] = 0x8F;
                else if (curr_instr.op_code == VM_OPC_JGE) ctx.buffer[ctx.pos++] = 0x8D;
                *(int*)(ctx.buffer + ctx.pos) = 0; ctx.pos += 4;
            }
            ctx.complexity_limit_instr_x86_64 += is_jmp ? 3 : 4;
            break;
        }
        case VM_OPC_EXIT:
        case VM_OPC_EXIT_STATUS_CODE: {
            if (!check_space(&ctx, 30)) goto bad;
            if (curr_instr.op_code == VM_OPC_EXIT) {
                struct x86_64 src = _get_x86_64_reg(curr_instr.arg0);
                struct x86_64 rax_reg = {0, 0}; 
                ctx.buffer[ctx.pos++] = _make_rex_w(&src, &rax_reg); 
                ctx.buffer[ctx.pos++] = 0x89;
                ctx.buffer[ctx.pos++] = 0xC0 | (src.code << 3) | rax_reg.code;
            } else {
                if (curr_instr.imm == 0) {
                    ctx.buffer[ctx.pos++] = 0x48; ctx.buffer[ctx.pos++] = 0x31; ctx.buffer[ctx.pos++] = 0xC0; 
                } else {
                    ctx.buffer[ctx.pos++] = 0x48; ctx.buffer[ctx.pos++] = 0xC7; ctx.buffer[ctx.pos++] = 0xC0; 
                    *(int*)(ctx.buffer + ctx.pos) = curr_instr.imm; ctx.pos += 4;
                }
            }
            
            // Epilogue
            ctx.buffer[ctx.pos++] = 0x48; ctx.buffer[ctx.pos++] = 0x81; ctx.buffer[ctx.pos++] = 0xC4;
            *(int*)(ctx.buffer + ctx.pos) = 520; ctx.pos += 4;

            ctx.buffer[ctx.pos++] = 0x41; ctx.buffer[ctx.pos++] = 0x5F; 
            ctx.buffer[ctx.pos++] = 0x41; ctx.buffer[ctx.pos++] = 0x5E; 
            ctx.buffer[ctx.pos++] = 0x41; ctx.buffer[ctx.pos++] = 0x5D; 
            ctx.buffer[ctx.pos++] = 0x41; ctx.buffer[ctx.pos++] = 0x5C; 
            ctx.buffer[ctx.pos++] = 0x5B;                               
            ctx.buffer[ctx.pos++] = 0x5D;                               
            ctx.buffer[ctx.pos++] = 0xC3;                               
            ctx.complexity_limit_instr_x86_64 += 3;
            break;
        }
        case VM_OPC_EXEC_FUNCTION: 
            if (emit_exec_function(&ctx, &curr_instr) < 0) goto bad; 
            break;
        default: goto bad;
        }
    }

    // Epilogue - VM_EXEC_ERROR_LIMIT_INSTRUCTIONS
    if (!check_space(&ctx, 25)) goto bad;
    ctx.error_exit_offset = ctx.pos;
    ctx.buffer[ctx.pos++] = 0x48; ctx.buffer[ctx.pos++] = 0xC7; ctx.buffer[ctx.pos++] = 0xC0;
    *(int*)(ctx.buffer + ctx.pos) = VM_EXEC_ERROR_LIMIT_INSTRUCTIONS; ctx.pos += 4;
    
    ctx.buffer[ctx.pos++] = 0x48; ctx.buffer[ctx.pos++] = 0x81; ctx.buffer[ctx.pos++] = 0xC4;
    *(int*)(ctx.buffer + ctx.pos) = 520; ctx.pos += 4; 
    
    ctx.buffer[ctx.pos++] = 0x41; ctx.buffer[ctx.pos++] = 0x5F;
    ctx.buffer[ctx.pos++] = 0x41; ctx.buffer[ctx.pos++] = 0x5E;
    ctx.buffer[ctx.pos++] = 0x41; ctx.buffer[ctx.pos++] = 0x5D;
    ctx.buffer[ctx.pos++] = 0x41; ctx.buffer[ctx.pos++] = 0x5C;
    ctx.buffer[ctx.pos++] = 0x5B; 
    ctx.buffer[ctx.pos++] = 0x5D; 
    ctx.buffer[ctx.pos++] = 0xC3;

    // Epilogue - VM_EXEC_ERROR_DIV_ZERO
    if (!check_space(&ctx, 25)) goto bad;
    ctx.div_zero_exit_offset = ctx.pos;
    ctx.buffer[ctx.pos++] = 0x48; ctx.buffer[ctx.pos++] = 0xC7; ctx.buffer[ctx.pos++] = 0xC0;
    *(int*)(ctx.buffer + ctx.pos) = VM_EXEC_ERROR_DIV_ZERO; ctx.pos += 4;

    ctx.buffer[ctx.pos++] = 0x48; ctx.buffer[ctx.pos++] = 0x81; ctx.buffer[ctx.pos++] = 0xC4;
    *(int*)(ctx.buffer + ctx.pos) = 520; ctx.pos += 4; 

    ctx.buffer[ctx.pos++] = 0x41; ctx.buffer[ctx.pos++] = 0x5F;
    ctx.buffer[ctx.pos++] = 0x41; ctx.buffer[ctx.pos++] = 0x5E;
    ctx.buffer[ctx.pos++] = 0x41; ctx.buffer[ctx.pos++] = 0x5D;
    ctx.buffer[ctx.pos++] = 0x41; ctx.buffer[ctx.pos++] = 0x5C;
    ctx.buffer[ctx.pos++] = 0x5B;
    ctx.buffer[ctx.pos++] = 0x5D;
    ctx.buffer[ctx.pos++] = 0xC3;

    for (int i = 0; i < instr_count; i++) {
        struct instruction curr = instr[i];
        int start = pc_to_buffer_offset[i];

        if (curr.op_code >= VM_OPC_JMP && curr.op_code <= VM_OPC_JGE) {
            int is_jmp = (curr.op_code == VM_OPC_JMP);
            
            int complexity_imm_offset = start + 9;
            *(int*)(ctx.buffer + complexity_imm_offset) = ctx.error_exit_offset - (complexity_imm_offset + 4);

            int target_pc = i + curr.imm;
            int target_buffer_offset = pc_to_buffer_offset[target_pc];

            if (is_jmp) {
                int jmp_imm_offset = start + 14;
                *(int*)(ctx.buffer + jmp_imm_offset) = target_buffer_offset - (jmp_imm_offset + 4);
            } else {
                int cond_imm_offset = start + 18;
                *(int*)(ctx.buffer + cond_imm_offset) = target_buffer_offset - (cond_imm_offset + 4);
            }
        }
    }

    // Backpatching division by zero 
    for (int k = 0; k < div_fixup_count; k++) {
        int fixup_pos = div_fixups[k];
        *(int*)(ctx.buffer + fixup_pos) = ctx.div_zero_exit_offset - (fixup_pos + 4);
    }

    if (ctx.complexity_limit_instr_x86_64 > VM_COMPLEXITY_LIMIT_INSTRUCTIONS) {
        goto bad;
    }

    kfree(div_fixups);
    kfree(pc_to_buffer_offset);
    return ctx.pos;

bad:
    if (div_fixups) kfree(div_fixups);
    if (pc_to_buffer_offset) kfree(pc_to_buffer_offset);
    return -1;
}