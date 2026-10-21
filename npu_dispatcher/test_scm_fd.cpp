// Тест передачи fd через SCM_RIGHTS
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <linux/dma-heap.h>
#include <thread>

#define SOCKET_PATH "/tmp/test_scm.sock"

int create_dma_buf(size_t size, void **ptr_out) {
    int heap_fd = open("/dev/dma_heap/reserved", O_RDWR);
    struct dma_heap_allocation_data data = { .len = size, .fd_flags = O_RDWR | O_CLOEXEC };
    if (ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &data) < 0) { perror("ALLOC"); return -1; }
    close(heap_fd);
    *ptr_out = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, data.fd, 0);
    return data.fd;
}

int send_fd(int sock, int fd) {
    char buf[1] = {'F'};
    struct iovec io = { .iov_base = buf, .iov_len = 1 };
    char cmsg_buf[CMSG_SPACE(sizeof(int))];
    struct msghdr msg = {0};
    msg.msg_iov = &io;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsg_buf;
    msg.msg_controllen = sizeof(cmsg_buf);
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));
    return sendmsg(sock, &msg, 0);
}

int recv_fd(int sock) {
    char buf[1];
    struct iovec io = { .iov_base = buf, .iov_len = 1 };
    char cmsg_buf[CMSG_SPACE(sizeof(int))];
    struct msghdr msg = {0};
    msg.msg_iov = &io;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsg_buf;
    msg.msg_controllen = sizeof(cmsg_buf);
    if (recvmsg(sock, &msg, 0) < 0) return -1;
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (cmsg && cmsg->cmsg_type == SCM_RIGHTS) {
        int fd;
        memcpy(&fd, CMSG_DATA(cmsg), sizeof(int));
        return fd;
    }
    return -1;
}

int main() {
    unlink(SOCKET_PATH);

    // Сервер в отдельном потоке
    std::thread server([]() {
        int srv = socket(AF_UNIX, SOCK_STREAM, 0);
        struct sockaddr_un addr = {0};
        addr.sun_family = AF_UNIX;
        strcpy(addr.sun_path, SOCKET_PATH);
        bind(srv, (struct sockaddr*)&addr, sizeof(addr));
        listen(srv, 1);
        printf("[server] listening\n");

        int cli = accept(srv, NULL, NULL);
        printf("[server] accepted\n");

        int fd = recv_fd(cli);
        printf("[server] received fd=%d\n", fd);

        void *ptr = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 0);
        unsigned char *p = (unsigned char*)ptr;
        printf("[server] first 8 bytes: ");
        for (int i = 0; i < 8; i++) printf("%02X ", p[i]);
        printf("\n");

        close(fd);
        close(cli);
        close(srv);
    });

    sleep(1);  // Ждём сервер

    // Клиент
    int cli = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, SOCKET_PATH);
    connect(cli, (struct sockaddr*)&addr, sizeof(addr));
    printf("[client] connected\n");

    size_t size = 4096;
    void *ptr;
    int fd = create_dma_buf(size, &ptr);
    printf("[client] created dma-buf fd=%d\n", fd);

    memset(ptr, 0xAB, size);
    printf("[client] wrote 0xAB\n");

    send_fd(cli, fd);
    printf("[client] sent fd\n");

    close(fd);
    close(cli);
    server.join();

    printf("DONE\n");
    return 0;
}
