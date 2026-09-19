#include "kernel_vm_s.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

int main(int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <id> <path_to_vasm_file> <operation>\n", argv[0]);
        fprintf(stderr, "Operations: write, execute, delete, all\n");
        fprintf(stderr, "Note: for 'execute' and 'delete', you can pass '-' as the path.\n");
        return 1;
    }

    char *endptr;
    unsigned long id = strtoul(argv[1], &endptr, 10);
    if (*endptr != '\0') {
        fprintf(stderr, "[-] Invalid ID format: %s\n", argv[1]);
        return 1;
    }

    const char *vasm_path = argv[2];
    
    const char *operation = argv[3];

    int fd = open(DEV_VM_PATH, O_RDWR);
    if (fd == -1) {
        perror("[-] Device open failed");
        return 1;
    }

    int op_handled = 0;

    if (strcmp(operation, "write") == 0 || strcmp(operation, "all") == 0) {
        op_handled = 1;
        const char *bin_path = "/tmp/temp_vm.bin";
        char cmd_buffer[512];
        snprintf(cmd_buffer, sizeof(cmd_buffer), "./vasm_compiler/compiler.py %s %s", vasm_path, bin_path);

        printf("[*] Compiling: %s\n", cmd_buffer);
        if (system(cmd_buffer) != 0) {
            fprintf(stderr, "[-] Compilation failed\n");
            close(fd);
            return 1;
        }

        FILE *f = fopen(bin_path, "rb");
        if (!f) {
            perror("[-] Failed to open binary file");
            close(fd);
            return 1;
        }

        fseek(f, 0, SEEK_END);
        long fsize = ftell(f);
        rewind(f);

        void *buffer = malloc(fsize);
        if (!buffer) {
            perror("[-] Malloc failed");
            fclose(f);
            close(fd);
            return 1;
        }
        
        fread(buffer, 1, fsize, f);
        fclose(f);
        remove(bin_path);

        struct dev_vm_packet p_write = { 
            .id = id, 
            .cmd = DEV_VM_PACKET_CMD_WRITE, 
            .data = buffer, 
            .data_size = (unsigned long)fsize 
        };
        
        if (write(fd, &p_write, sizeof(p_write)) < 0) {
            perror("[-] CMD_WRITE failed");
        } else {
            printf("[+] JIT compiled and saved (ID: %lu)\n", id);
        }
        free(buffer);
    }

    if (strcmp(operation, "execute") == 0 || strcmp(operation, "all") == 0) {
        op_handled = 1;
        struct dev_vm_packet p_exec = { .id = id, .cmd = DEV_VM_PACKET_CMD_EXECUTE };
        int ret = write(fd, &p_exec, sizeof(p_exec));
        if (ret < 0) {
            perror("[-] CMD_EXECUTE failed");
        } else {
            printf("[+] VM executed. Return code: %d\n", ret);
        }
    }

    if (strcmp(operation, "delete") == 0 || strcmp(operation, "all") == 0) {
        op_handled = 1;
        struct dev_vm_packet p_del = { .id = id, .cmd = DEV_VM_PACKET_CMD_DELETE };
        if (write(fd, &p_del, sizeof(p_del)) < 0) {
            perror("[-] CMD_DELETE failed");
        } else {
            printf("[+] Memory cleaned up (ID: %lu)\n", id);
        }
    }

    if (!op_handled) {
        fprintf(stderr, "[-] Unknown operation: %s\n", operation);
        fprintf(stderr, "[*] Supported: write, execute, delete, all\n");
    }

    close(fd);
    return 0;
}