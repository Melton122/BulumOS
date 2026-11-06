// SPDX-License-Identifier: GPL-2.0
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mmap.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stdint.h>
#include <pthread.h>
#include <string.h>

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t bpp;
} fb_info_t;

typedef struct {
    uint32_t type;
    uint32_t code;
    uint32_t value;
} input_event_t;

typedef struct {
    int x, y, w, h;
    uint32_t color;
    char title[32];
} window_t;

uint32_t *fbp;
fb_info_t info;
int mouse_x = 100, mouse_y = 100;
window_t windows[10];
int window_count = 0;

void add_window(int x, int y, int w, int h, uint32_t color, const char* title) {
    if (window_count < 10) {
        windows[window_count] = (window_t){x, y, w, h, color, ""};
        strncpy(windows[window_count].title, title, 31);
        window_count++;
    }
}

void *input_thread(void *arg) {
    int input_fd = open("/dev/input0", O_RDONLY);
    if (input_fd < 0) return NULL;
    input_event_t ev;
    while (read(input_fd, &ev, sizeof(ev)) > 0) {
        if (ev.type == 1) {
            if (ev.code == 0) mouse_x += ev.value;
            if (ev.code == 1) mouse_y += ev.value;
        }
    }
    return NULL;
}

void *wayland_socket_thread(void *arg) {
    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, "/tmp/wayland-0", sizeof(addr.sun_path)-1);
    unlink(addr.sun_path);

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) return NULL;
    listen(server_fd, 5);
    printf("Wayland socket listening on /tmp/wayland-0\n");

    while(1) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) continue;
        printf("New Wayland client connected!\n");
        
        // Basic Wayland-like Handshake: Send a fake welcome
        char welcome[] = "WELCOME_TO_BULUMOS_WAYLAND";
        write(client_fd, welcome, sizeof(welcome));
        
        // For now, we just create a window for every connection
        add_window(200 + (window_count*50), 200 + (window_count*50), 200, 150, 0x00FF00, "Wayland App");
        close(client_fd);
    }
    return NULL;
}

int main() {
    int fb_fd = open("/dev/fb0", O_RDWR);
    if (fb_fd < 0) return 1;
    if (ioctl(fb_fd, 0, &info) < 0) return 1;

    size_t screensize = info.width * info.height * 4;
    fbp = (uint32_t *)mmap(0, screensize, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if ((intptr_t)fbp == -1) return 1;

    pthread_t tid_in, tid_way;
    pthread_create(&tid_in, NULL, input_thread, NULL);
    pthread_create(&tid_way, NULL, wayland_socket_thread, NULL);

    add_window(50, 50, 400, 300, 0x333333, "BulumOS Shell");

    while(1) {
        for (int i = 0; i < info.width * info.height; i++) fbp[i] = 0x000022;
        for (int i = 0; i < window_count; i++) {
            window_t *w = &windows[i];
            for (int wy = w->y; wy < w->y + w->h && wy < info.height; wy++) {
                for (int wx = w->x; wx < w->x + w->w && wx < info.width; wx++) {
                    fbp[wy * info.width + wx] = w->color;
                }
            }
        }
        for(int y = mouse_y; y < mouse_y + 5 && y < info.height; y++) {
            for(int x = mouse_x; x < mouse_x + 5 && x < info.width; x++) {
                fbp[y * info.width + x] = 0xFFFFFF;
            }
        }
        usleep(16000);
    }
    return 0;
}
