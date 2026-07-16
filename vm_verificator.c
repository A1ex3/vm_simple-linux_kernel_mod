#include "vm_verificator.h"

static int _check_register_num(unsigned char arg) {
    if (arg >= MAX_REGS) {
        return 0;
    }
    return 1;
}

static int _check_registers_values(
    unsigned char arg0, unsigned char arg1, unsigned char arg2, long long imm,
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

        if (curr_instr.op_code > VM_LAST_OP_CODE_INSTR_NUM || curr_instr.op_code == 0) {
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

            if (curr_instr.imm != (long long)(int)curr_instr.imm) return VERIFI_ERROR_OUT_OF_PROGRAMM;

            int offset = (int)curr_instr.imm;
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