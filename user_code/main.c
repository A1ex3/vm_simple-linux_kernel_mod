#include "kernel_vm_s.h"
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>

VM_INIT_CODE(my_code,
    VM_CODE(VM_OPC_MOV, VM_REG1, 0, 0, 5)
    VM_CODE(VM_OPC_MOV, VM_REG2, 0, 0, 20)
    VM_CODE(VM_OPC_ADD, VM_REG0, VM_REG1, VM_REG2, 0)
    VM_CODE(VM_OPC_EXEC_FUNCTION, 0, 0, 0, VM_FUNCTION_PRINTINT)
    VM_CODE(VM_OPC_EXIT_STATUS_CODE, 0, 0, 0, 0)
);

int main() {
    unsigned long id = 42; 
    int fd = open(DEV_VM_PATH, O_RDWR);
    if (fd == -1) {
        perror("[-] Device open failed");
        return 1;
    }

    // 1. WRITE
    struct dev_vm_packet p_write = { .id = id, .cmd = DEV_VM_PACKET_CMD_WRITE, .data = VM_GET_BYTECODE(my_code), .data_size = VM_CODE_INSTRUCTIONS_COUNT(my_code) };
    if (write(fd, &p_write, sizeof(p_write)) < 0) perror("[-] CMD_WRITE failed");
    else printf("[+] JIT compiled and saved\n");

    // 2. EXECUTE
    struct dev_vm_packet p_exec = { .id = id, .cmd = DEV_VM_PACKET_CMD_EXECUTE };
    int ret = write(fd, &p_exec, sizeof(p_exec));
    if (ret < 0) perror("[-] CMD_EXECUTE failed");
    else printf("[+] VM executed. Return code: %d\n", ret);

    // 3. DELETE
    struct dev_vm_packet p_del = { .id = id, .cmd = DEV_VM_PACKET_CMD_DELETE };
    if (write(fd, &p_del, sizeof(p_del)) < 0) perror("[-] CMD_DELETE failed");
    else printf("[+] Memory cleaned up\n");

    close(fd);
    return 0;
}