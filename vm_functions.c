#include "vm_functions.h"

#include <linux/printk.h>
#include <asm/barrier.h>
#include <linux/net.h>
#include <linux/in.h>
#include <linux/socket.h>
#include <net/sock.h>
#include <linux/ip.h>
#include <linux/icmp.h>
#include <linux/jiffies.h>

static int vm_function_printint(int n) {
    printk(KERN_INFO "%d\n", n);
    return 0;
}

static void vm_function_push_k2u(long long value) {
    unsigned int next_head = (k2u_rb->head + 1) % RING_BUFFER_CAPACITY;
    
    if (next_head != k2u_rb->tail) {
        k2u_rb->data[k2u_rb->head] = value;
        smp_wmb();
        k2u_rb->head = next_head;
    }
}

static long long vm_function_pop_u2k(void) {
    if (u2k_rb->head == u2k_rb->tail) return 0;

    long long val = u2k_rb->data[u2k_rb->tail];
    smp_rmb();
    
    u2k_rb->tail = (u2k_rb->tail + 1) % RING_BUFFER_CAPACITY;
    return val;
}

static int vm_function_net_ping(unsigned long ip) {
    struct socket *sock;
    struct sockaddr_in sin;
    struct msghdr msg;
    struct kvec iov;
    struct icmphdr icmp_hdr;
    int ret;

    ret = sock_create_kern(&init_net, PF_INET, SOCK_RAW, IPPROTO_ICMP, &sock);
    if (ret < 0) {
        printk(KERN_ERR "VM: Failed to create ICMP socket: %d\n", ret);
        return ret;
    }

    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl((unsigned int)ip);

    memset(&icmp_hdr, 0, sizeof(icmp_hdr));
    icmp_hdr.type = ICMP_ECHO;
    icmp_hdr.code = 0;
    icmp_hdr.un.echo.id = htons(1234);
    icmp_hdr.un.echo.sequence = htons(1);
    icmp_hdr.checksum = 0;
    icmp_hdr.checksum = ip_compute_csum(&icmp_hdr, sizeof(icmp_hdr));

    iov.iov_base = &icmp_hdr;
    iov.iov_len = sizeof(icmp_hdr);

    memset(&msg, 0, sizeof(msg));
    msg.msg_name = &sin;
    msg.msg_namelen = sizeof(sin);

    ret = kernel_sendmsg(sock, &msg, &iov, 1, sizeof(icmp_hdr));
    if (ret < 0) {
        printk(KERN_ERR "VM: ICMP Ping failed to send (Network down?): %d\n", ret);
        sock_release(sock);
        return ret;
    }
    printk(KERN_INFO "VM: Request sent to %pI4, waiting for reply...\n", &sin.sin_addr.s_addr);

    sock->sk->sk_rcvtimeo = msecs_to_jiffies(2000);

    while (1) {
        char recv_buf[128];
        struct kvec riov = { .iov_base = recv_buf, .iov_len = sizeof(recv_buf) };
        struct msghdr rmsg;
        memset(&rmsg, 0, sizeof(rmsg));

        ret = kernel_recvmsg(sock, &rmsg, &riov, 1, sizeof(recv_buf), 0);

        if (ret == -EAGAIN || ret == -EWOULDBLOCK) {
            printk(KERN_ERR "VM: Server %pI4 is down (Timeout)\n", &sin.sin_addr.s_addr);
            ret = -ETIMEDOUT;
            break;
        } else if (ret < 0) {
            printk(KERN_ERR "VM: Receive error: %d\n", ret);
            break;
        }

        struct iphdr *iph = (struct iphdr *)recv_buf;
        
        if (iph->protocol == IPPROTO_ICMP) {
            struct icmphdr *icmph = (struct icmphdr *)(recv_buf + (iph->ihl * 4));

            if (icmph->type == ICMP_ECHOREPLY) {
                if (icmph->un.echo.id == htons(1234)) {
                    printk(KERN_INFO "VM: Server is ALIVE! Reply from %pI4\n", &sin.sin_addr.s_addr);
                    ret = 0;
                    break;
                }
            } 

            else if (icmph->type == ICMP_DEST_UNREACH) {
                printk(KERN_ERR "VM: Destination Unreachable for %pI4\n", &sin.sin_addr.s_addr);
                ret = -EHOSTUNREACH;
                break;
            }
        }
    }

    sock_release(sock);
    return ret;
}

void* vm_functions[VM_FUNCTIONS_COUNT] = {
    [VM_FUNCTION_PRINTINT] = (void*)vm_function_printint,
    [VM_FUNCTION_PUSH_RING_BUFFER_K2U] = (void*)vm_function_push_k2u,
    [VM_FUNCTION_POP_RING_BUFFER_U2K] = (void*)vm_function_pop_u2k,
    [VM_FUNCTION_NET_PING] = (void*)vm_function_net_ping,
};
