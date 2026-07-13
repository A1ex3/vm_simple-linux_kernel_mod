#ifndef __KERNEL_VM_S_H__
#define __KERNEL_VM_S_H__

struct instruction {
    unsigned char       op_code;
    unsigned char       arg0;
    unsigned char       arg1;
    unsigned char       arg2;
    long long           imm;
} __attribute__((packed));

#define MAX_REGS 8

#define VM_REG0  0
#define VM_REG1  1
#define VM_REG2  2
#define VM_REG3  3
#define VM_REG4  4
#define VM_REG5  5
#define VM_REG6  6
#define VM_REGSP 7

#define VM_OPC_ADD 1
#define VM_OPC_SUB 2
#define VM_OPC_MUL 3
#define VM_OPC_DIVIDE 4
#define VM_OPC_MOV 5
#define VM_OPC_MOV_REG 6
#define VM_OPC_JMP 7
#define VM_OPC_JEQ 8   // == (Jump if Equal)
#define VM_OPC_JNE 9   // != (Jump if Not Equal)
#define VM_OPC_JLT 10  // <  (Jump if Less Than)
#define VM_OPC_JLE 11  // <= (Jump if Less or Equal)
#define VM_OPC_JGT 12  // >  (Jump if Greater Than)
#define VM_OPC_JGE 13  // >= (Jump if Greater or Equal)
#define VM_OPC_EXIT 14
#define VM_OPC_EXIT_STATUS_CODE 15
#define VM_OPC_EXEC_FUNCTION 16 // func(REG0, REG1, REG2, REG3, REG4, REG5)
#define VM_OPC_STORE_STACK 17
#define VM_OPC_LOAD_STACK  18

#define VM_LAST_OP_CODE_INSTR_NUM VM_OPC_LOAD_STACK

#define VM_EXEC_ERROR_LIMIT_INSTRUCTIONS -2
#define VM_EXEC_ERROR_DIV_ZERO -3

#define VERIFI_OK 0
#define VERIFI_ERROR_REGISTER -1
#define VERIFI_ERROR_DIVIDE_ZERO -2
#define VERIFI_ERROR_OUT_OF_PROGRAMM -3
#define VERIFI_ERROR_OUT_OF_SIZE_TYPE -4
#define VERIFI_ERROR_INCORRECT_OP_CODE -5
#define VERIFI_ERROR_REGISTERS_VALUES -6
#define VERIFI_ERROR_COMPLEXITY_LIMIT_INSTRUCTIONS -7
#define VERIFI_ERROR_INFINITE_LOOP -8
#define VERIFI_ERROR_FUNCTION_DOESNT_EXISTS -9
#define VERIFI_ERROR_STACK_ALIGNMENT -10
#define VERIFI_ERROR_STACK_OUT_OF_BOUNDS -11
#define VERIFI_ERROR_READ_ONLY_REGISTER -12

#define VM_GET_BYTECODE(name) _vm_bytecode_##name

#define VM_INIT_CODE(name, array) \
    unsigned char VM_GET_BYTECODE(name)[] = { array }

#define VM_CODE_INSTRUCTIONS_COUNT(name) \
    (sizeof(_vm_bytecode_##name))

#define VM_CODE(op_code, arg0, arg1, arg2, imm) \
    (op_code), \
    (arg0), \
    (arg1), \
    (arg2), \
    (unsigned char)(((unsigned long long)(imm)) & 0xFF), \
    (unsigned char)((((unsigned long long)(imm)) >> 8) & 0xFF), \
    (unsigned char)((((unsigned long long)(imm)) >> 16) & 0xFF), \
    (unsigned char)((((unsigned long long)(imm)) >> 24) & 0xFF), \
    (unsigned char)((((unsigned long long)(imm)) >> 32) & 0xFF), \
    (unsigned char)((((unsigned long long)(imm)) >> 40) & 0xFF), \
    (unsigned char)((((unsigned long long)(imm)) >> 48) & 0xFF), \
    (unsigned char)((((unsigned long long)(imm)) >> 56) & 0xFF),

enum vm_cmd_functions {
    VM_FUNCTION_PRINTINT = 0,
    VM_FUNCTION_PUSH_RING_BUFFER_K2U,
    VM_FUNCTION_POP_RING_BUFFER_U2K,
    VM_FUNCTIONS_COUNT
};

#define DEV_VM_NAME "vm_s"
#define DEV_VM_PATH "/dev/vm_s"

#define DEV_VM_PACKET_CMD_WRITE 1
#define DEV_VM_PACKET_CMD_EXECUTE 2
#define DEV_VM_PACKET_CMD_DELETE 3

struct dev_vm_packet {
    int cmd;
    unsigned long id;
    unsigned long data_size;
    unsigned char* data;
} __attribute__((packed));

#define RING_BUFFER_CAPACITY 512

struct rb_to_kernel {
    volatile unsigned int head;
    volatile unsigned int tail;
    long long data[RING_BUFFER_CAPACITY - 1];
};

struct rb_to_user {
    volatile unsigned int head;
    volatile unsigned int tail;
    long long data[RING_BUFFER_CAPACITY - 1];
};

extern struct rb_to_user *k2u_rb;
extern struct rb_to_kernel *u2k_rb;

#endif