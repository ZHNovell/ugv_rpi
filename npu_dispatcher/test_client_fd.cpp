// Тестовый клиент: создаёт dma-buf 640×640 с шумом, отправляет fd в NPU-сервер
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
#include <arpa/inet.h>
#include <opencv2/opencv.hpp>

#define SOCKET_PATH "/tmp/npu_yolo_fd.sock"
#define W 640
#define H 640

int create_dma_buf(size_t size, void **ptr_out) {
    int heap_fd = open("/dev/dma_heap/reserved", O_RDWR);
    size = (size + 63) & ~63;
    struct dma_heap_allocation_data data = { .len = size, .fd_flags = O_RDWR | O_CLOEXEC };
    if (ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &data) < 0) return -1;
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

bool read_n(int fd, void* buf, size_t n) {
    char *p = (char*)buf;
    while (n > 0) {
        ssize_t r = read(fd, p, n);
        if (r <= 0) return false;
        p += r; n -= r;
    }
    return true;
}

int main() {
    // Соединяемся с сервером
    int cli = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, SOCKET_PATH);
    if (connect(cli, (struct sockaddr*)&addr, sizeof(addr)) < 0) { perror("connect"); return 1; }
    printf("Connected\n");

    // Создаём dma-buf 640×640×3
    size_t size = W * H * 3;
    void *ptr;
    int fd = create_dma_buf(size, &ptr);
    printf("dma-buf fd=%d, ptr=%p\n", fd, ptr);

    // Заполняем тестовым паттерном (серый + шум)
    cv::Mat img(H, W, CV_8UC3);
    cv::randu(img, cv::Scalar(0, 0, 0), cv::Scalar(255, 255, 255));
    memcpy(ptr, img.data, size);
    printf("Image filled with noise\n");

    // Отправляем fd
    if (send_fd(cli, fd) < 0) { perror("send_fd"); return 1; }
    printf("fd sent\n");

    // Принимаем JSON
    uint32_t json_len_be;
    if (!read_n(cli, &json_len_be, 4)) { printf("read json_len failed\n"); return 1; }
    uint32_t json_len = ntohl(json_len_be);
    printf("json_len=%u\n", json_len);

    std::vector<char> json(json_len + 1);
    if (!read_n(cli, json.data(), json_len)) { printf("read json failed\n"); return 1; }
    json[json_len] = '\0';
    printf("JSON: %s\n", json.data());

    close(fd);
    close(cli);
    printf("DONE\n");
    return 0;
}
