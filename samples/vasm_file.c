#include "kernel_vm_s.h"

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

int main() {
    system("./vasm_compiler/compiler.py samples/printint_example.vasm samples/printint_example.bin");

    FILE *f = fopen("samples/printint_example.bin", "rb");
    if (!f) {
        perror("[-] Failed to open binary file");
        return 1;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    rewind(f);

    void *buffer = malloc(fsize);
    if (!buffer) {
        perror("[-] Malloc failed");
        fclose(f);
        return 1;
    }
    fread(buffer, 1, fsize, f);
    fclose(f);

    unsigned long id = 11; 
    int fd = open(DEV_VM_PATH, O_RDWR);
    if (fd == -1) {
        perror("[-] Device open failed");
        free(buffer);
        return 1;
    }

    // WRITE
    struct dev_vm_packet p_write = { 
        .id = id, 
        .cmd = DEV_VM_PACKET_CMD_WRITE, 
        .data = buffer, 
        .data_size = (unsigned long)fsize 
    };
    
    if (write(fd, &p_write, sizeof(p_write)) < 0) {
        perror("[-] CMD_WRITE failed");
    } else {
        printf("[+] JIT compiled and saved\n");
    }

    // EXECUTE
    struct dev_vm_packet p_exec = { .id = id, .cmd = DEV_VM_PACKET_CMD_EXECUTE };
    int ret = write(fd, &p_exec, sizeof(p_exec));
    if (ret < 0) perror("[-] CMD_EXECUTE failed");
    else printf("[+] VM executed. Return code: %d\n", ret);

    // DELETE
    struct dev_vm_packet p_del = { .id = id, .cmd = DEV_VM_PACKET_CMD_DELETE };
    if (write(fd, &p_del, sizeof(p_del)) < 0) perror("[-] CMD_DELETE failed");
    else printf("[+] Memory cleaned up\n");

    close(fd);
    free(buffer);
    
    return 0;
}