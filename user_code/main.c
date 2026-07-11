#include "kernel_vm_s.h"
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/mman.h>

#define RB_REAL_CAPACITY (RING_BUFFER_CAPACITY - 1)
#define WAIT_TIMEOUT 100000

VM_INIT_CODE(my_code,
    VM_CODE(VM_OPC_MOV, VM_REG3, 0, 0, 3)
    VM_CODE(VM_OPC_MOV, VM_REG4, 0, 0, 0)
    VM_CODE(VM_OPC_MOV, VM_REG1, 0, 0, 1)

    VM_CODE(VM_OPC_JEQ, VM_REG3, VM_REG4, 0, 9)

    VM_CODE(VM_OPC_MOV_REG, VM_REG0, VM_REG4, 0, 0)
    VM_CODE(VM_OPC_EXEC_FUNCTION, 0, 0, 0, VM_FUNCTION_POP_RING_BUFFER_U2K)
    VM_CODE(VM_OPC_MOV_REG, VM_REG6, VM_REG0, 0, 0)
    VM_CODE(VM_OPC_MUL, VM_REG2, VM_REG6, VM_REG6, 0)
    VM_CODE(VM_OPC_MOV_REG, VM_REG0, VM_REG2, 0, 0)
    VM_CODE(VM_OPC_EXEC_FUNCTION, 0, 0, 0, VM_FUNCTION_PUSH_RING_BUFFER_K2U)

    VM_CODE(VM_OPC_SUB, VM_REG3, VM_REG3, VM_REG1, 0)
    VM_CODE(VM_OPC_JMP, 0, 0, 0, -8)

    VM_CODE(VM_OPC_EXIT_STATUS_CODE, 0, 0, 0, 0)
);

int main() {
    int fd = open(DEV_VM_PATH, O_RDWR);
    if (fd == -1) {
        perror("[-] Device open failed");
        return 1;
    }

    size_t page_size = getpagesize();

    struct rb_to_user *rb_k2u = mmap(NULL, page_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0 * page_size);
    struct rb_to_kernel *rb_u2k = mmap(NULL, page_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 1 * page_size);

    if (rb_k2u == MAP_FAILED || rb_u2k == MAP_FAILED) {
        perror("[-] mmap failed");
        close(fd);
        return 1;
    }

    // Load
    struct dev_vm_packet p_write = {
        .cmd = DEV_VM_PACKET_CMD_WRITE,
        .id = 41,
        .data_size = VM_CODE_INSTRUCTIONS_COUNT(my_code),
        .data = VM_GET_BYTECODE(my_code)
    };

    if (write(fd, &p_write, sizeof(p_write)) < 0) {
        perror("[-] Failed to write bytecode");
        return 1;
    }

    // Feed input data (1, 2, 3) into U2K ring buffer
    for (long long i = 1; i <= 3; i++) {
        unsigned int next = (rb_u2k->head + 1) % RB_REAL_CAPACITY;
        if (next != rb_u2k->tail) {
            rb_u2k->data[rb_u2k->head] = i;
            rb_u2k->head = next;
        } else {
            fprintf(stderr, "[-] U2K ring buffer is full\n");
        }
    }

    // Execute
    struct dev_vm_packet p_exec = {
        .cmd = DEV_VM_PACKET_CMD_EXECUTE,
        .id = 41,
        .data_size = 0,
        .data = NULL
    };

    if (write(fd, &p_exec, sizeof(p_exec)) < 0) {
        perror("[-] Execution failed");
        return 1;
    }

    printf("[*] Waiting for VM output...\n");
    int received = 0;
    for (int i = 0; i < 3; i++) {
        int timeout = WAIT_TIMEOUT;
        while (rb_k2u->head == rb_k2u->tail) {
            if (--timeout == 0) {
                fprintf(stderr, "[-] Timeout waiting for K2U data\n");
                munmap(rb_k2u, page_size);
                munmap(rb_u2k, page_size);
                close(fd);
                return 1;
            }
            usleep(1000);
        }

        long long val = rb_k2u->data[rb_k2u->tail];
        printf("[+] Received from K2U (squared): %lld\n", val);

        rb_k2u->tail = (rb_k2u->tail + 1) % RB_REAL_CAPACITY;
        received++;
    }

    if (received != 3) {
        fprintf(stderr, "[-] Received only %d of 3 expected values\n", received);
    }

    // Delete
    struct dev_vm_packet p_del = {
        .cmd = DEV_VM_PACKET_CMD_DELETE,
        .id = 41,
        .data_size = 0,
        .data = NULL
    };
    write(fd, &p_del, sizeof(p_del));

    munmap(rb_k2u, page_size);
    munmap(rb_u2k, page_size);
    close(fd);
    return 0;
}