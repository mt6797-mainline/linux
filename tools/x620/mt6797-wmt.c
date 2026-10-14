// SPDX-License-Identifier: GPL-2.0-only
/* MT6797 stock STP/WMT userspace command and ROM patch-info protocol. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define WMT_SET_MODE _IOW(0xa0, 5, int)
#define WMT_FUNC_CTRL _IOW(0xa0, 6, int)
#define WMT_GET_CHIP_INFO _IOR(0xa0, 12, int)
#define WMT_LAUNCHER_KILL _IOW(0xa0, 13, int)
#define WMT_PATCH_NUM _IOW(0xa0, 14, int)
#define WMT_PATCH_INFO _IOW(0xa0, 15, char *)
#define MAX_PATCHES 15
struct patch_info {
    uint32_t sequence;
    uint8_t address[4];
    char path[256];
};
_Static_assert(sizeof(struct patch_info) == 264, "WMT patch ioctl ABI");
struct patch {
    struct patch_info info;
    unsigned int total, version;
};
static volatile sig_atomic_t stopping;
static void stop(int sig) { (void)sig; stopping = 1; }

static int scan_patches(const char *directory, struct patch *patches, int fw_version)
{
    struct dirent *entry;
    unsigned int mask = 0, total = 0;
    uint8_t set_header[24] = {0};
    DIR *dir = opendir(directory);
    if (!dir) return -1;
    while ((entry = readdir(dir))) {
        char path[PATH_MAX];
        uint8_t header[28];
        int fd;
        ssize_t count;
        unsigned int seq, num, version;
        if (strncmp(entry->d_name, "ROMv3_patch_", 12) ||
            !strstr(entry->d_name, "_hdr.bin")) continue;
        if (snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name) >= (int)sizeof(path)) {
            errno = ENAMETOOLONG; goto fail;
        }
        fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) goto fail;
        count = read(fd, header, sizeof(header));
        close(fd);
        if (count != sizeof(header)) { errno = EINVAL; goto fail; }
        version = ((unsigned int)header[22] << 8) | header[23];
        /* The stock launcher compares the low ROM version byte. */
        if (fw_version >= 0 && ((version ^ fw_version) & 0xff)) continue;
        num = header[24] >> 4;
        seq = header[24] & 15;
        if (!num || num > MAX_PATCHES || !seq || seq > num ||
            (total && total != num) || (mask & (1U << (seq - 1)))) {
            errno = EINVAL; goto fail;
        }
        if (mask && memcmp(set_header, header, sizeof(set_header))) {
            errno = EINVAL; goto fail;
        }
        memcpy(set_header, header, sizeof(set_header));
        total = num;
        if (strlen(path) >= sizeof(patches[seq - 1].info.path)) {
            errno = ENAMETOOLONG; goto fail;
        }
        patches[seq - 1].info.sequence = seq;
        memcpy(patches[seq - 1].info.address, header + 24, 4);
        patches[seq - 1].info.address[0] = 0;
        strcpy(patches[seq - 1].info.path, path);
        patches[seq - 1].version = version;
        patches[seq - 1].total = num;
        mask |= 1U << (seq - 1);
    }
    closedir(dir);
    if (!total || mask != (1U << total) - 1) { errno = ENOENT; return -1; }
    return total;
fail:
    closedir(dir);
    return -1;
}

static int patch_request(int fd, const char *directory)
{
    struct patch patches[MAX_PATCHES] = {0};
    int chip = ioctl(fd, WMT_GET_CHIP_INFO, 0UL);
    int hw = ioctl(fd, WMT_GET_CHIP_INFO, 1UL);
    int fw = ioctl(fd, WMT_GET_CHIP_INFO, 2UL);
    int count, i;
    if (chip < 0 || hw < 0 || fw < 0) return -1;
    fprintf(stderr, "MT6797 WMT: chip=%04x hw=%04x ROM=%04x\n", chip, hw, fw);
    if (chip != 0x0279) { errno = ENODEV; return -1; }
    count = scan_patches(directory, patches, fw);
    if (count < 0) return -1;
    if (ioctl(fd, WMT_PATCH_NUM, (unsigned long)count) < 0) return -1;
    for (i = 0; i < count; i++) {
        fprintf(stderr, "WMT patch %u/%d %s address=%02x%02x%02x%02x\n",
                patches[i].info.sequence, count, patches[i].info.path,
                patches[i].info.address[3], patches[i].info.address[2],
                patches[i].info.address[1], patches[i].info.address[0]);
        if (ioctl(fd, WMT_PATCH_INFO, &patches[i].info) < 0) return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *device = "/dev/stpwmt";
    int fd, ret = 0;
    if (argc == 3 && !strcmp(argv[1], "--inspect")) {
        struct patch patches[MAX_PATCHES] = {0};
        int n = scan_patches(argv[2], patches, -1), i;
        if (n < 0) { perror("patch validation"); return 1; }
        for (i = 0; i < n; i++)
            printf("seq=%u total=%u version=%04x address=%02x%02x%02x%02x %s\n",
                   patches[i].info.sequence, patches[i].total, patches[i].version,
                   patches[i].info.address[3], patches[i].info.address[2],
                   patches[i].info.address[1], patches[i].info.address[0], patches[i].info.path);
        return 0;
    }
    if (argc < 2 || (strcmp(argv[1], "--on") && strcmp(argv[1], "--off") &&
                     strcmp(argv[1], "--serve"))) {
        fprintf(stderr, "usage: %s --serve firmware-directory [stpwmt-device] | --on | --off | --inspect directory\n", argv[0]);
        return 2;
    }
    if (!strcmp(argv[1], "--serve") && argc < 3) return 2;
    if (argc > 3) device = argv[3];
    fd = open(device, O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror(device); return 1; }
    if (!strcmp(argv[1], "--on") || !strcmp(argv[1], "--off")) {
        unsigned long op = !strcmp(argv[1], "--on") ? 0x80000003UL : 3UL;
        ret = ioctl(fd, WMT_FUNC_CTRL, op);
        if (ret < 0) perror("WMT WLAN function control");
        close(fd);
        return ret < 0 ? 1 : 0;
    }
    setvbuf(stderr, NULL, _IONBF, 0);
    signal(SIGINT, stop); signal(SIGTERM, stop);
    /* BTIF full framing (3), FM over common transport (2): stock HIF configuration. */
    if (ioctl(fd, WMT_SET_MODE, 0x23UL) < 0 || ioctl(fd, WMT_LAUNCHER_KILL, 0UL) < 0) {
        perror("WMT BTIF setup"); close(fd); return 1;
    }
    fprintf(stderr, "MT6797_WMT_LAUNCHER_READY\n");
    while (!stopping) {
        struct pollfd p = {.fd = fd, .events = POLLIN};
        char command[256] = {0};
        ssize_t n;
        ret = poll(&p, 1, 1000);
        if (ret < 0 && errno == EINTR) continue;
        if (ret < 0) { perror("WMT poll"); break; }
        if (!ret) continue;
        if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) { ret = -1; break; }
        n = read(fd, command, sizeof(command) - 1);
        if (n <= 0) { if (n < 0) perror("WMT command read"); ret = -1; break; }
        command[n] = 0;
        fprintf(stderr, "WMT request: %s\n", command);
        ret = !strcmp(command, "srh_patch") ? patch_request(fd, argv[2]) : -1;
        if (ret < 0) perror("WMT request failed");
        /* No newline: the driver compares the entire reply to the exact string "ok". */
        if (write(fd, ret < 0 ? "error" : "ok", ret < 0 ? 5 : 2) < 0) {
            perror("WMT command reply"); ret = -1; break;
        }
    }
    ioctl(fd, WMT_LAUNCHER_KILL, 1UL);
    close(fd);
    return ret < 0 ? 1 : 0;
}
