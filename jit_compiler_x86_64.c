#include "jit_compiler_x86_64.h"
#include "vm_functions.h"

#include <linux/kernel.h>
#include <linux/slab.h>

#define VM_JIT_STACK_FRAME_SIZE 512

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
    int* complexity_fixups;
    int  complexity_fixup_count;
    int* jump_fixups;
    int* jump_target_pc;
    int  jump_fixup_count;
    int* div_fixups;
    int  div_fixup_count;
};

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
        case VM_OPC_DIVIDE:                    return 15;
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
        case VM_OPC_EXEC_FUNCTION:             return 28;
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

static int emit_divide(struct jit_ctx* ctx, struct instruction* instr) {
    if (!check_space(ctx, 50)) return -1;
    
    struct x86_64 dst = _get_x86_64_reg(instr->arg0);
    struct x86_64 src1 = _get_x86_64_reg(instr->arg1);
    struct x86_64 src2 = _get_x86_64_reg(instr->arg2);
    struct x86_64 rax_reg = {0, 0}; 

    // test src2, src2; jz div_zero
    ctx->buffer[ctx->pos++] = 0x48 | src2.is_ext;
    ctx->buffer[ctx->pos++] = 0x85;
    ctx->buffer[ctx->pos++] = 0xC0 | (src2.code << 3) | src2.code;

    ctx->buffer[ctx->pos++] = 0x0F; ctx->buffer[ctx->pos++] = 0x84;
    ctx->div_fixups[ctx->div_fixup_count++] = ctx->pos;
    *(int*)(ctx->buffer + ctx->pos) = 0; ctx->pos += 4;

    // cmp src2, -1
    ctx->buffer[ctx->pos++] = 0x48 | src2.is_ext;
    ctx->buffer[ctx->pos++] = 0x83;
    ctx->buffer[ctx->pos++] = 0xF8 | src2.code;
    ctx->buffer[ctx->pos++] = 0xFF;

    // jne safe_div
    ctx->buffer[ctx->pos++] = 0x75; 
    ctx->buffer[ctx->pos++] = 19; 

    // mov rax, 0x8000000000000000 (INT64_MIN)
    ctx->buffer[ctx->pos++] = 0x48; ctx->buffer[ctx->pos++] = 0xB8;
    *(unsigned long long*)(ctx->buffer + ctx->pos) = 0x8000000000000000ULL;
    ctx->pos += 8;

    // cmp src1, rax
    ctx->buffer[ctx->pos++] = 0x48 | src1.is_ext;
    ctx->buffer[ctx->pos++] = 0x39;
    ctx->buffer[ctx->pos++] = 0xC0 | src1.code;

    // je div_zero
    ctx->buffer[ctx->pos++] = 0x0F; ctx->buffer[ctx->pos++] = 0x84;
    ctx->div_fixups[ctx->div_fixup_count++] = ctx->pos;
    *(int*)(ctx->buffer + ctx->pos) = 0; ctx->pos += 4;

    // safe_div:
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

    ctx->complexity_limit_instr_x86_64 += 15; 
    return 0;
}

static int emit_mov(struct jit_ctx* ctx, struct instruction* instr) {
    if (!check_space(ctx, 10)) return -1;
    struct x86_64 dst = _get_x86_64_reg(instr->arg0);
    ctx->buffer[ctx->pos++] = 0x48 | dst.is_ext;
    ctx->buffer[ctx->pos++] = 0xB8 | dst.code;
    *(long long*)(ctx->buffer + ctx->pos) = instr->imm; ctx->pos += 8;
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
    
    *(int*)(ctx->buffer + ctx->pos) = (int)instr->imm;
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
    
    *(int*)(ctx->buffer + ctx->pos) = (int)instr->imm;
    ctx->pos += 4;
    
    ctx->complexity_limit_instr_x86_64 += 1;
    return 0;
}

static int emit_exec_function(struct jit_ctx* ctx, struct instruction* instr) {
    struct x86_64 abi_regs[] = { {7, 0}, {6, 0}, {2, 0}, {1, 0}, {0, 1}, {1, 1} };
    int abi_regs_size = sizeof(abi_regs) / sizeof(abi_regs[0]);

    if (!check_space(ctx, 110)) return -1;
    
    ctx->buffer[ctx->pos++] = 0x41; ctx->buffer[ctx->pos++] = 0x50; // push r8
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
    *(unsigned long long*)(ctx->buffer + ctx->pos) = (unsigned long long)vm_functions[instr->imm];
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
    ctx->buffer[ctx->pos++] = 0x41; ctx->buffer[ctx->pos++] = 0x58; // pop r8

    struct x86_64 target_reg = _get_x86_64_reg(instr->arg0);
    struct x86_64 rax_reg = {0, 0};
    ctx->buffer[ctx->pos++] = _make_rex_w(&rax_reg, &target_reg);
    ctx->buffer[ctx->pos++] = 0x89;
    ctx->buffer[ctx->pos++] = 0xC0 | (rax_reg.code << 3) | target_reg.code;

    ctx->complexity_limit_instr_x86_64 += 28;
    return 0;
}

int jit_x86_64_compiler(struct instruction* instr, int instr_count, unsigned char* buffer, int buffer_size) {
    struct jit_ctx ctx = { .buffer = buffer, .buffer_size = buffer_size, .pos = 0, .complexity_limit_instr_x86_64 = 0 };

    int* pc_to_buffer_offset = (int*)kzalloc(instr_count * sizeof(int), GFP_KERNEL);

    ctx.complexity_fixups = (int*)kzalloc(instr_count * sizeof(int), GFP_KERNEL);
    ctx.jump_fixups       = (int*)kzalloc(instr_count * sizeof(int), GFP_KERNEL);
    ctx.jump_target_pc    = (int*)kzalloc(instr_count * sizeof(int), GFP_KERNEL);
    ctx.div_fixups        = (int*)kzalloc(instr_count * sizeof(int), GFP_KERNEL);
    ctx.complexity_fixup_count = 0;
    ctx.jump_fixup_count = 0;
    ctx.div_fixup_count = 0;

    if (!pc_to_buffer_offset || !ctx.complexity_fixups || !ctx.jump_fixups
        || !ctx.jump_target_pc || !ctx.div_fixups) goto bad;

    // Prologue
    if (!check_space(&ctx, 65)) goto bad;
    ctx.buffer[ctx.pos++] = 0x55;                               // push rbp
    ctx.buffer[ctx.pos++] = 0x53;                               // push rbx
    ctx.buffer[ctx.pos++] = 0x41; ctx.buffer[ctx.pos++] = 0x54; // push r12
    ctx.buffer[ctx.pos++] = 0x41; ctx.buffer[ctx.pos++] = 0x55; // push r13
    ctx.buffer[ctx.pos++] = 0x41; ctx.buffer[ctx.pos++] = 0x56; // push r14
    ctx.buffer[ctx.pos++] = 0x41; ctx.buffer[ctx.pos++] = 0x57; // push r15

    // sub rsp, VM_JIT_STACK_FRAME_SIZE
    ctx.buffer[ctx.pos++] = 0x48; ctx.buffer[ctx.pos++] = 0x81; ctx.buffer[ctx.pos++] = 0xEC;
    *(int*)(ctx.buffer + ctx.pos) = VM_JIT_STACK_FRAME_SIZE; ctx.pos += 4;
    
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

    // mov r8, VM_COMPLEXITY_LIMIT
    ctx.buffer[ctx.pos++] = 0x49; ctx.buffer[ctx.pos++] = 0xB8;
    *(unsigned long*)(ctx.buffer + ctx.pos) = VM_COMPLEXITY_LIMIT_INSTRUCTIONS; ctx.pos += 8;
    ctx.complexity_limit_instr_x86_64 += 13;

    for (int i = 0; i < instr_count; i++) {
        struct instruction curr_instr = instr[i];
        pc_to_buffer_offset[i] = ctx.pos;

        switch (curr_instr.op_code) {
        case VM_OPC_ADD: if (emit_add_sub(&ctx, &curr_instr, 0) < 0) goto bad; break;
        case VM_OPC_SUB: if (emit_add_sub(&ctx, &curr_instr, 1) < 0) goto bad; break;
        case VM_OPC_MUL: if (emit_mul(&ctx, &curr_instr) < 0) goto bad; break;
        case VM_OPC_DIVIDE: if (emit_divide(&ctx, &curr_instr) < 0) goto bad; break;
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
                for (int j = i + (int)curr_instr.imm; j <= i; j++) 
                    native_cost += _jit_x86_64_compiler_get_native_instruction_count(instr[j].op_code);
            } else {
                native_cost = _jit_x86_64_compiler_get_native_instruction_count(curr_instr.op_code);
            }

            // sub r8, native_cost
            ctx.buffer[ctx.pos++] = 0x49; ctx.buffer[ctx.pos++] = 0x81; ctx.buffer[ctx.pos++] = 0xE8;
            *(int*)(ctx.buffer + ctx.pos) = native_cost; ctx.pos += 4;

            // jle error_exit
            ctx.buffer[ctx.pos++] = 0x0F; ctx.buffer[ctx.pos++] = 0x8E;
            ctx.complexity_fixups[ctx.complexity_fixup_count++] = ctx.pos;
            *(int*)(ctx.buffer + ctx.pos) = 0; ctx.pos += 4;

            int target_pc = i + (int)curr_instr.imm;

            if (is_jmp) {
                ctx.buffer[ctx.pos++] = 0xE9;
                ctx.jump_fixups[ctx.jump_fixup_count] = ctx.pos;
                ctx.jump_target_pc[ctx.jump_fixup_count] = target_pc;
                ctx.jump_fixup_count++;
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
                ctx.jump_fixups[ctx.jump_fixup_count] = ctx.pos;
                ctx.jump_target_pc[ctx.jump_fixup_count] = target_pc;
                ctx.jump_fixup_count++;
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
                    *(int*)(ctx.buffer + ctx.pos) = (int)curr_instr.imm; ctx.pos += 4;
                }
            }
            
            // Epilogue
            ctx.buffer[ctx.pos++] = 0x48; ctx.buffer[ctx.pos++] = 0x81; ctx.buffer[ctx.pos++] = 0xC4;
            *(int*)(ctx.buffer + ctx.pos) = VM_JIT_STACK_FRAME_SIZE; ctx.pos += 4;

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
    *(int*)(ctx.buffer + ctx.pos) = VM_JIT_STACK_FRAME_SIZE; ctx.pos += 4; 
    
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
    *(int*)(ctx.buffer + ctx.pos) = VM_JIT_STACK_FRAME_SIZE; ctx.pos += 4; 

    ctx.buffer[ctx.pos++] = 0x41; ctx.buffer[ctx.pos++] = 0x5F;
    ctx.buffer[ctx.pos++] = 0x41; ctx.buffer[ctx.pos++] = 0x5E;
    ctx.buffer[ctx.pos++] = 0x41; ctx.buffer[ctx.pos++] = 0x5D;
    ctx.buffer[ctx.pos++] = 0x41; ctx.buffer[ctx.pos++] = 0x5C;
    ctx.buffer[ctx.pos++] = 0x5B;
    ctx.buffer[ctx.pos++] = 0x5D;
    ctx.buffer[ctx.pos++] = 0xC3;

    // Backpatch: complexity-limit jumps
    for (int f = 0; f < ctx.complexity_fixup_count; f++) {
        int fixup_pos = ctx.complexity_fixups[f];
        *(int*)(ctx.buffer + fixup_pos) = ctx.error_exit_offset - (fixup_pos + 4);
    }

    // Backpatch: jmp/jcc branch targets
    for (int f = 0; f < ctx.jump_fixup_count; f++) {
        int fixup_pos = ctx.jump_fixups[f];
        int target_pc = ctx.jump_target_pc[f];
        int target_buffer_offset = pc_to_buffer_offset[target_pc];
        *(int*)(ctx.buffer + fixup_pos) = target_buffer_offset - (fixup_pos + 4);
    }

    // Backpatch: division by zero
    for (int k = 0; k < ctx.div_fixup_count; k++) {
        int fixup_pos = ctx.div_fixups[k];
        *(int*)(ctx.buffer + fixup_pos) = ctx.div_zero_exit_offset - (fixup_pos + 4);
    }

    if (ctx.complexity_limit_instr_x86_64 > VM_COMPLEXITY_LIMIT_INSTRUCTIONS) {
        goto bad;
    }

    kfree(ctx.div_fixups);
    kfree(ctx.jump_target_pc);
    kfree(ctx.jump_fixups);
    kfree(ctx.complexity_fixups);
    kfree(pc_to_buffer_offset);
    return ctx.pos;

bad:
    if (ctx.div_fixups) kfree(ctx.div_fixups);
    if (ctx.jump_target_pc) kfree(ctx.jump_target_pc);
    if (ctx.jump_fixups) kfree(ctx.jump_fixups);
    if (ctx.complexity_fixups) kfree(ctx.complexity_fixups);
    if (pc_to_buffer_offset) kfree(pc_to_buffer_offset);
    return -1;
}