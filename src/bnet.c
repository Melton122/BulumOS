/*
 * bnet - BulumOS network interface (Phase 7)
 *
 * Native C networking front-end. Uses Linux APIs (getifaddrs, ioctl,
 * /proc/net) and existing BusyBox tools (udhcpc, ping) via fixed-path
 * execve with controlled arguments. No system(), no popen().
 *
 * Commands:
 *   bnet status                     overall summary
 *   bnet list                       interface names
 *   bnet info <iface>               interface details
 *   bnet up <iface>                 bring interface up
 *   bnet down <iface>               bring interface down
 *   bnet ip <iface>                 show addresses
 *   bnet ip add <addr/prefix> <if>  assign IPv4 address
 *   bnet ip del <addr/prefix> <if>  remove IPv4 address
 *   bnet route                      show routing table
 *   bnet route add <dest> <gw> [iface]
 *   bnet route del <dest> <gw> [iface]
 *   bnet dns                        show DNS configuration
 *   bnet dns set <ns>...            set nameservers in /etc/resolv.conf
 *   bnet dhcp <iface>               acquire a lease via udhcpc
 *   bnet ping <host>                ping (fixed /bin/ping)
 *   bnet resolve <host>             getaddrinfo()
 *   bnet sockets                    listening sockets + owners
 *   bnet apply                      apply /etc/bulum/network.conf
 */

#define _GNU_SOURCE

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <net/route.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define BNET_VERSION "1.0.0"
#define NETCONF "/etc/bulum/network.conf"
#define RESOLV  "/etc/resolv.conf"
#define UDHCPC  "/sbin/udhcpc"
#define UDHCP_SCRIPT "/usr/share/udhcpc/default.script"
#define PING    "/bin/ping"
#define UDHCP_PIDFILE "/var/run/bulum-network.pid"

#define MAX_IFNAME (IFNAMSIZ - 1)
#define MAX_DNS 8

/* ---------- helpers ---------- */

static int valid_ifname(const char *name)
{
    size_t n = strlen(name);
    if (n == 0 || n > MAX_IFNAME)
        return 0;
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-'))
            return 0;
    }
    return 1;
}

static int iface_exists(const char *name)
{
    char path[64];
    int n = snprintf(path, sizeof(path), "/sys/class/net/%.*s", 15,
                     name);
    return n > 0 && access(path, F_OK) == 0;
}

static int if_flags(const char *name)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%.*s",
             (int)sizeof(ifr.ifr_name) - 1, name);
    if (ioctl(fd, SIOCGIFFLAGS, &ifr) < 0) {
        close(fd);
        return -1;
    }
    close(fd);
    return ifr.ifr_flags;
}

static int set_if_flag(const char *name, short flag, int on)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%.*s",
             (int)sizeof(ifr.ifr_name) - 1, name);
    if (ioctl(fd, SIOCGIFFLAGS, &ifr) < 0) {
        close(fd);
        return -1;
    }
    if (on)
        ifr.ifr_flags |= flag;
    else
        ifr.ifr_flags &= (short)~flag;
    int rc = ioctl(fd, SIOCSIFFLAGS, &ifr);
    close(fd);
    return rc;
}

static int if_mtu(const char *name)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%.*s",
             (int)sizeof(ifr.ifr_name) - 1, name);
    if (ioctl(fd, SIOCGIFMTU, &ifr) < 0) {
        close(fd);
        return -1;
    }
    close(fd);
    return ifr.ifr_mtu;
}

static int if_mac(const char *name, unsigned char mac[6])
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%.*s",
             (int)sizeof(ifr.ifr_name) - 1, name);
    if (ioctl(fd, SIOCGIFHWADDR, &ifr) < 0) {
        close(fd);
        return -1;
    }
    close(fd);
    memcpy(mac, ifr.ifr_hwaddr.sa_data, 6);
    return 0;
}

static const char *operstate(const char *name)
{
    static char buf[32];
    char path[64];
    snprintf(path, sizeof(path), "/sys/class/net/%.*s/operstate", 15,
             name);
    FILE *f = fopen(path, "r");
    if (!f)
        return "unknown";
    if (!fgets(buf, sizeof(buf), f)) {
        fclose(f);
        return "unknown";
    }
    fclose(f);
    buf[strcspn(buf, "\r\n")] = 0;
    return buf;
}

/* collect IPv4 addresses + netmask per interface via getifaddrs */
typedef struct {
    char name[MAX_IFNAME + 1];
    struct in_addr addr;
    struct in_addr mask;
    int has_v4;
} v4ent_t;

typedef struct {
    char name[MAX_IFNAME + 1];
    struct in6_addr addr;
    int prefix;
    int scope;
    int has_v6;
} v6ent_t;

static v4ent_t v4tab[64];
static int nv4;
static v6ent_t v6tab[64];
static int nv6;

static void scan_addrs(void)
{
    nv4 = nv6 = 0;
    struct ifaddrs *ifa, *p;
    if (getifaddrs(&ifa) != 0)
        return;
    for (p = ifa; p && nv4 < 64 && nv6 < 64; p = p->ifa_next) {
        if (!p->ifa_addr || !p->ifa_name)
            continue;
        if (p->ifa_addr->sa_family == AF_INET) {
            v4ent_t *e = &v4tab[nv4];
            snprintf(e->name, sizeof(e->name), "%s", p->ifa_name);
            e->addr = ((struct sockaddr_in *)p->ifa_addr)->sin_addr;
            if (p->ifa_netmask)
                e->mask = ((struct sockaddr_in *)p->ifa_netmask)->sin_addr;
            else
                e->mask.s_addr = 0;
            e->has_v4 = 1;
            nv4++;
        } else if (p->ifa_addr->sa_family == AF_INET6) {
            v6ent_t *e = &v6tab[nv6];
            snprintf(e->name, sizeof(e->name), "%s", p->ifa_name);
            e->addr = ((struct sockaddr_in6 *)p->ifa_addr)->sin6_addr;
            e->scope = ((struct sockaddr_in6 *)p->ifa_addr)->sin6_scope_id;
            e->prefix = 64;
            if (p->ifa_netmask) {
                const unsigned char *m =
                    (const unsigned char *)&((struct sockaddr_in6 *)
                        p->ifa_netmask)->sin6_addr;
                int bits = 0;
                for (int i = 0; i < 16 && m[i] == 0xff; i++)
                    bits += 8;
                if (bits < 128) {
                    unsigned char last = m[bits / 8];
                    while (last & 0x80) {
                        bits++;
                        last <<= 1;
                    }
                }
                e->prefix = bits;
            }
            e->has_v6 = 1;
            nv6++;
        }
    }
    freeifaddrs(ifa);
}

static const v4ent_t *iface_v4(const char *name)
{
    for (int i = 0; i < nv4; i++)
        if (strcmp(v4tab[i].name, name) == 0)
            return &v4tab[i];
    return NULL;
}

/* ---------- commands ---------- */

static int cmd_list(void)
{
    DIR *d = opendir("/sys/class/net");
    if (!d) {
        perror("bnet: cannot open /sys/class/net");
        return 1;
    }
    struct dirent *e;
    char names[64][MAX_IFNAME + 1];
    int n = 0;
    while ((e = readdir(d)) && n < 64) {
        if (e->d_name[0] == '.')
            continue;
        snprintf(names[n], sizeof(names[0]), "%.*s",
                 (int)sizeof(names[0]) - 1, e->d_name);
        n++;
    }
    closedir(d);
    /* deterministic sort */
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (strcmp(names[j], names[i]) < 0) {
                char tmp[MAX_IFNAME + 1];
                memcpy(tmp, names[i], sizeof(tmp));
                memcpy(names[i], names[j], sizeof(names[i]));
                memcpy(names[j], tmp, sizeof(tmp));
            }
    for (int i = 0; i < n; i++)
        printf("%s\n", names[i]);
    return 0;
}

static int cmd_info(const char *name)
{
    if (!valid_ifname(name)) {
        fprintf(stderr, "bnet: invalid interface name '%s'\n", name);
        return 2;
    }
    if (!iface_exists(name)) {
        fprintf(stderr, "bnet: interface '%s' does not exist\n", name);
        return 3;
    }
    int flags = if_flags(name);
    int up = flags >= 0 && (flags & IFF_UP);
    int running = flags >= 0 && (flags & IFF_RUNNING);
    unsigned char mac[6];
    char macs[32] = "none";
    if (if_mac(name, mac) == 0 &&
        !(mac[0] == 0 && mac[1] == 0 && mac[2] == 0 && mac[3] == 0 &&
          mac[4] == 0 && mac[5] == 0)) {
        snprintf(macs, sizeof(macs),
                 "%02x:%02x:%02x:%02x:%02x:%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    printf("Interface: %s\n", name);
    printf("State: %s\n", up ? "UP" : "DOWN");
    printf("Link: %s\n", running ? "up" : "down");
    printf("MAC: %s\n", macs);
    scan_addrs();
    const v4ent_t *v4 = iface_v4(name);
    if (v4) {
        char a[64], m[64];
        inet_ntop(AF_INET, &v4->addr, a, sizeof(a));
        inet_ntop(AF_INET, &v4->mask, m, sizeof(m));
        unsigned long nm = ntohl(v4->mask.s_addr);
        int prefix = 0;
        while (prefix < 32 && (nm & (1UL << (31 - prefix))))
            prefix++;
        printf("IPv4: %s/%d\n", a, prefix);
        (void)m;
    } else {
        printf("IPv4: none\n");
    }
    for (int i = 0; i < nv6; i++) {
        if (strcmp(v6tab[i].name, name) == 0) {
            char a[64];
            inet_ntop(AF_INET6, &v6tab[i].addr, a, sizeof(a));
            printf("IPv6: %s/%d\n", a, v6tab[i].prefix);
        }
    }
    int mtu = if_mtu(name);
    printf("MTU: %d\n", mtu >= 0 ? mtu : 0);
    printf("Operstate: %s\n", operstate(name));
    return 0;
}

static int cmd_up(const char *name)
{
    if (!valid_ifname(name)) {
        fprintf(stderr, "bnet: invalid interface name '%s'\n", name);
        return 2;
    }
    if (!iface_exists(name)) {
        fprintf(stderr, "bnet: interface '%s' does not exist\n", name);
        return 3;
    }
    if (set_if_flag(name, IFF_UP, 1) < 0) {
        fprintf(stderr, "bnet: cannot bring '%s' up: %s\n", name,
                strerror(errno));
        return 1;
    }
    return 0;
}

static int cmd_down(const char *name)
{
    if (!valid_ifname(name)) {
        fprintf(stderr, "bnet: invalid interface name '%s'\n", name);
        return 2;
    }
    if (!iface_exists(name)) {
        fprintf(stderr, "bnet: interface '%s' does not exist\n", name);
        return 3;
    }
    if (set_if_flag(name, IFF_UP, 0) < 0) {
        fprintf(stderr, "bnet: cannot bring '%s' down: %s\n", name,
                strerror(errno));
        return 1;
    }
    return 0;
}

static int cmd_ip(const char *name)
{
    if (!valid_ifname(name)) {
        fprintf(stderr, "bnet: invalid interface name '%s'\n", name);
        return 2;
    }
    if (!iface_exists(name)) {
        fprintf(stderr, "bnet: interface '%s' does not exist\n", name);
        return 3;
    }
    scan_addrs();
    const v4ent_t *v4 = iface_v4(name);
    if (v4) {
        char a[64];
        inet_ntop(AF_INET, &v4->addr, a, sizeof(a));
        unsigned long nm = ntohl(v4->mask.s_addr);
        int prefix = 0;
        while (prefix < 32 && (nm & (1UL << (31 - prefix))))
            prefix++;
        printf("%s %s/%d\n", name, a, prefix);
    } else {
        printf("%s: no IPv4 address\n", name);
    }
    return v4 ? 0 : 1;
}

/* parse "a.b.c.d" or "a.b.c.d/nn" -> addr, prefix */
static int parse_addr(const char *s, struct in_addr *addr, int *prefix)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "%s", s);
    char *slash = strchr(buf, '/');
    if (slash) {
        *slash = 0;
        *prefix = atoi(slash + 1);
        if (*prefix < 0 || *prefix > 32)
            return -1;
    } else {
        *prefix = 32;
    }
    return inet_pton(AF_INET, buf, addr) == 1 ? 0 : -1;
}

static int cmd_ip_add(const char *addr_s, const char *name)
{
    if (!valid_ifname(name)) {
        fprintf(stderr, "bnet: invalid interface name '%s'\n", name);
        return 2;
    }
    if (!iface_exists(name)) {
        fprintf(stderr, "bnet: interface '%s' does not exist\n", name);
        return 3;
    }
    struct in_addr addr;
    int prefix;
    if (parse_addr(addr_s, &addr, &prefix) != 0) {
        fprintf(stderr, "bnet: invalid address '%s' (use addr/prefix)\n",
                addr_s);
        return 2;
    }
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("bnet: socket");
        return 1;
    }
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%.*s",
             (int)sizeof(ifr.ifr_name) - 1, name);
    struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
    sin->sin_family = AF_INET;
    sin->sin_addr = addr;
    if (ioctl(fd, SIOCSIFADDR, &ifr) < 0) {
        fprintf(stderr, "bnet: cannot set address: %s\n", strerror(errno));
        close(fd);
        return 1;
    }
    sin->sin_addr.s_addr = prefix ? htonl(0xffffffffUL << (32 - prefix)) : 0;
    if (ioctl(fd, SIOCSIFNETMASK, &ifr) < 0) {
        fprintf(stderr, "bnet: cannot set netmask: %s\n", strerror(errno));
        close(fd);
        return 1;
    }
    close(fd);
    set_if_flag(name, IFF_UP, 1);
    return 0;
}

static int cmd_ip_del(const char *addr_s, const char *name)
{
    if (!valid_ifname(name)) {
        fprintf(stderr, "bnet: invalid interface name '%s'\n", name);
        return 2;
    }
    if (!iface_exists(name)) {
        fprintf(stderr, "bnet: interface '%s' does not exist\n", name);
        return 3;
    }
    struct in_addr addr;
    int prefix;
    if (parse_addr(addr_s, &addr, &prefix) != 0) {
        fprintf(stderr, "bnet: invalid address '%s'\n", addr_s);
        return 2;
    }
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("bnet: socket");
        return 1;
    }
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%.*s",
             (int)sizeof(ifr.ifr_name) - 1, name);
    struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
    sin->sin_family = AF_INET;
    sin->sin_addr = addr;
    if (ioctl(fd, SIOCDIFADDR, &ifr) < 0) {
        fprintf(stderr, "bnet: cannot delete address: %s\n", strerror(errno));
        close(fd);
        return 1;
    }
    close(fd);
    return 0;
}

/* ---------- routing ---------- */

static void print_rt4(void)
{
    FILE *f = fopen("/proc/net/route", "r");
    if (!f)
        return;
    char line[256];
    if (!fgets(line, sizeof(line), f)) { /* header */
        fclose(f);
        return;
    }
    printf("%-18s %-18s %s\n", "Destination", "Gateway", "Iface");
    while (fgets(line, sizeof(line), f)) {
        char iface[32] = "";
        unsigned long dest = 0, gw = 0, mask = 0;
        int flags = 0;
        if (sscanf(line, "%31s %lx %lx %x %*d %*d %*d %lx", iface, &dest, &gw,
                   &flags, &mask) != 5)
            continue;
        char d[64], g[64];
        struct in_addr da = { .s_addr = (uint32_t)dest };
        struct in_addr ga = { .s_addr = (uint32_t)gw };
        inet_ntop(AF_INET, &da, d, sizeof(d));
        inet_ntop(AF_INET, &ga, g, sizeof(g));
        int prefix = 0;
        uint32_t m = ntohl((uint32_t)mask);
        while (prefix < 32 && (m & (1UL << (31 - prefix))))
            prefix++;
        if (dest == 0)
            printf("%-18s ", "default");
        else
            printf("%s/%d    ", d, prefix);
        printf("%-18s %s\n", gw == 0 ? "*" : g, iface);
    }
    fclose(f);
}

static void print_rt6(void)
{
    FILE *f = fopen("/proc/net/ipv6_route", "r");
    if (!f)
        return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char iface[32] = "";
        char dhex[33] = "", ghex[33] = "";
        unsigned int dplen = 0, gplen = 0;
        unsigned long metric = 0, flags = 0;
        if (sscanf(line, "%32s %x %32s %x %lx %lx %31s",
                   dhex, &dplen, ghex, &gplen, &metric, &flags, iface) != 7)
            continue;
        struct in6_addr d, g;
        if (inet_pton(AF_INET6, dhex, &d) != 1 ||
            inet_pton(AF_INET6, ghex, &g) != 1)
            continue;
        char ds[64], gs[64];
        inet_ntop(AF_INET6, &d, ds, sizeof(ds));
        inet_ntop(AF_INET6, &g, gs, sizeof(gs));
        int all_zero = 1;
        for (int i = 0; i < 16; i++)
            if (d.s6_addr[i])
                all_zero = 0;
        if (all_zero)
            printf("%-18s ", "default");
        else
            printf("%s/%d ", ds, dplen);
        int gz = 1;
        for (int i = 0; i < 16; i++)
            if (g.s6_addr[i])
                gz = 0;
        printf("%-18s %s\n", gz ? "*" : gs, iface);
    }
    fclose(f);
}

static int cmd_route(void)
{
    print_rt4();
    print_rt6();
    return 0;
}

static int cmd_route_add_del(int del, int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: bnet route %s <dest> <gateway> [iface]\n",
                del ? "del" : "add");
        return 2;
    }
    struct in_addr dest, gw;
    int prefix = 0;
    if (strcmp(argv[0], "default") == 0) {
        dest.s_addr = 0;
    } else {
        if (parse_addr(argv[0], &dest, &prefix) != 0) {
            fprintf(stderr, "bnet: invalid destination '%s'\n", argv[0]);
            return 2;
        }
        if (prefix != 32)
            return 2;
    }
    if (inet_pton(AF_INET, argv[1], &gw) != 1) {
        fprintf(stderr, "bnet: invalid gateway '%s'\n", argv[1]);
        return 2;
    }
    const char *iface = argc >= 3 ? argv[2] : NULL;
    if (iface && (!valid_ifname(iface) || !iface_exists(iface))) {
        fprintf(stderr, "bnet: bad interface '%s'\n", iface);
        return 2;
    }
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("bnet: socket");
        return 1;
    }
    struct rtentry rt;
    memset(&rt, 0, sizeof(rt));
    char rtdev[IFNAMSIZ] = "";
    if (iface) {
        snprintf(rtdev, sizeof(rtdev), "%.*s", IFNAMSIZ - 1, iface);
        rt.rt_dev = rtdev;
    }
    struct sockaddr_in *sd = (struct sockaddr_in *)&rt.rt_dst;
    sd->sin_family = AF_INET;
    sd->sin_addr = dest;
    struct sockaddr_in *sg = (struct sockaddr_in *)&rt.rt_gateway;
    sg->sin_family = AF_INET;
    sg->sin_addr = gw;
    struct sockaddr_in *sm = (struct sockaddr_in *)&rt.rt_genmask;
    sm->sin_family = AF_INET;
    if (dest.s_addr == 0) {
        rt.rt_flags = RTF_UP | RTF_GATEWAY;
        sm->sin_addr.s_addr = 0;
    } else {
        rt.rt_flags = RTF_UP | RTF_GATEWAY | RTF_HOST;
        sm->sin_addr.s_addr = 0xffffffff;
    }
    int rc = ioctl(fd, del ? SIOCDELRT : SIOCADDRT, &rt);
    close(fd);
    if (rc < 0) {
        fprintf(stderr, "bnet: route %s failed: %s\n",
                del ? "delete" : "add", strerror(errno));
        return 1;
    }
    return 0;
}

static const char *gateway4(void)
{
    static char buf[64];
    FILE *f = fopen("/proc/net/route", "r");
    if (!f)
        return NULL;
    char line[256];
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        return NULL;
    }
    while (fgets(line, sizeof(line), f)) {
        char iface[32] = "";
        unsigned long dest = 0, gw = 0;
        int flags = 0;
        if (sscanf(line, "%31s %lx %lx %x", iface, &dest, &gw, &flags) != 4)
            continue;
        if (dest == 0 && gw != 0) {
            struct in_addr ga = { .s_addr = (uint32_t)gw };
            inet_ntop(AF_INET, &ga, buf, sizeof(buf));
            fclose(f);
            return buf;
        }
    }
    fclose(f);
    return NULL;
}

/* ---------- DNS ---------- */

static int cmd_dns(void)
{
    FILE *f = fopen(RESOLV, "r");
    if (!f) {
        printf("DNS: no %s\n", RESOLV);
        return 0;
    }
    char line[256];
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (strncmp(line, "nameserver", 10) == 0) {
            printf("nameserver%s\n", line + 10);
            found = 1;
        } else if (strncmp(line, "search", 6) == 0) {
            printf("search%s\n", line + 6);
        }
    }
    fclose(f);
    if (!found)
        printf("nameserver (none configured)\n");
    return 0;
}

static int cmd_dns_set(int argc, char **argv)
{
    if (argc < 1) {
        fprintf(stderr, "usage: bnet dns set <nameserver>...\n");
        return 2;
    }
    if (argc > MAX_DNS) {
        fprintf(stderr, "bnet: too many nameservers (max %d)\n", MAX_DNS);
        return 2;
    }
    for (int i = 0; i < argc; i++) {
        struct in_addr a;
        struct in6_addr a6;
        if (inet_pton(AF_INET, argv[i], &a) != 1 &&
            inet_pton(AF_INET6, argv[i], &a6) != 1) {
            fprintf(stderr, "bnet: invalid nameserver '%s'\n", argv[i]);
            return 2;
        }
    }
    char tmp[128];
    snprintf(tmp, sizeof(tmp), "%s.tmp.%d", RESOLV, (int)getpid());
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        fprintf(stderr, "bnet: cannot write %s: %s\n", RESOLV,
                strerror(errno));
        return 1;
    }
    for (int i = 0; i < argc; i++) {
        dprintf(fd, "nameserver %s\n", argv[i]);
    }
    close(fd);
    if (rename(tmp, RESOLV) < 0) {
        unlink(tmp);
        fprintf(stderr, "bnet: cannot update %s: %s\n", RESOLV,
                strerror(errno));
        return 1;
    }
    return 0;
}

/* ---------- DHCP ---------- */

/* run udhcpc for iface; wait up to timeout_s; returns 0 if a lease was
 * obtained (interface has an IPv4 address), 1 on failure/timeout */
static int run_udhcpc(const char *iface, int timeout_s)
{
    pid_t pid = fork();
    if (pid < 0) {
        perror("bnet: fork");
        return 1;
    }
    if (pid == 0) {
        int fd = open("/dev/null", O_RDWR);
        if (fd >= 0) {
            dup2(fd, 0);
            dup2(fd, 1);
            dup2(fd, 2);
            if (fd > 2)
                close(fd);
        }
        char *argv[] = { (char *)UDHCPC, (char *)"-i", (char *)iface,
                         (char *)"-p", (char *)UDHCP_PIDFILE,
                         (char *)"-s", (char *)UDHCP_SCRIPT,
                         (char *)"-b", (char *)"-q", NULL };
        execv(UDHCPC, argv);
        _exit(127);
    }
    int status = 0;
    time_t t0 = time(NULL);
    for (;;) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid)
            break;
        if (time(NULL) - t0 > timeout_s) {
            kill(pid, SIGTERM);
            waitpid(pid, &status, 0);
            fprintf(stderr, "bnet: dhcp timed out after %ds\n", timeout_s);
            return 1;
        }
        usleep(200000);
    }
    if (!WIFEXITED(status)) {
        fprintf(stderr, "bnet: dhcp client failed\n");
        return 1;
    }
    scan_addrs();
    return iface_v4(iface) ? 0 : 1;
}

static int cmd_dhcp(const char *name)
{
    if (!valid_ifname(name)) {
        fprintf(stderr, "bnet: invalid interface name '%s'\n", name);
        return 2;
    }
    if (!iface_exists(name)) {
        fprintf(stderr, "bnet: interface '%s' does not exist\n", name);
        return 3;
    }
    set_if_flag(name, IFF_UP, 1);
    unlink(UDHCP_PIDFILE);
    int rc = run_udhcpc(name, 30);
    scan_addrs();
    const v4ent_t *v4 = iface_v4(name);
    if (rc == 0 && v4) {
        char a[64];
        inet_ntop(AF_INET, &v4->addr, a, sizeof(a));
        const char *gw = gateway4();
        printf("%s: %s\n", name, a);
        printf("Gateway: %s\n", gw ? gw : "(none)");
        printf("DNS: ");
        FILE *f = fopen(RESOLV, "r");
        if (f) {
            char line[256];
            int any = 0;
            while (fgets(line, sizeof(line), f)) {
                if (strncmp(line, "nameserver", 10) == 0) {
                    line[strcspn(line, "\r\n")] = 0;
                    printf("%s%s", any ? " " : "", line + 11);
                    any = 1;
                }
            }
            fclose(f);
            if (!any)
                printf("(none)");
            printf("\n");
        } else {
            printf("(none)\n");
        }
        return 0;
    }
    fprintf(stderr, "bnet: no DHCP lease on %s\n", name);
    return 1;
}

/* ---------- diagnostics ---------- */

static int cmd_ping(int argc, char **argv)
{
    if (argc < 1) {
        fprintf(stderr, "usage: bnet ping <host>\n");
        return 2;
    }
    const char *host = argv[0];
    if (strlen(host) > 255 || host[0] == '-') {
        fprintf(stderr, "bnet: invalid host '%s'\n", host);
        return 2;
    }
    pid_t pid = fork();
    if (pid < 0) {
        perror("bnet: fork");
        return 1;
    }
    if (pid == 0) {
        char *args[] = { (char *)PING, (char *)"-c", (char *)"3",
                         (char *)"-W", (char *)"1", (char *)host, NULL };
        execv(PING, args);
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return 1;
}

static int cmd_resolve(const char *host)
{
    if (strlen(host) > 255 || host[0] == '-') {
        fprintf(stderr, "bnet: invalid host '%s'\n", host);
        return 2;
    }
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int rc = getaddrinfo(host, NULL, &hints, &res);
    if (rc != 0) {
        fprintf(stderr, "bnet: cannot resolve '%s': %s\n", host,
                gai_strerror(rc));
        return 1;
    }
    int n = 0;
    for (struct addrinfo *p = res; p; p = p->ai_next) {
        char buf[128];
        if (p->ai_family == AF_INET) {
            inet_ntop(AF_INET,
                      &((struct sockaddr_in *)p->ai_addr)->sin_addr,
                      buf, sizeof(buf));
            printf("%s (IPv4)\n", buf);
        } else if (p->ai_family == AF_INET6) {
            inet_ntop(AF_INET6,
                      &((struct sockaddr_in6 *)p->ai_addr)->sin6_addr,
                      buf, sizeof(buf));
            printf("%s (IPv6)\n", buf);
        }
        n++;
    }
    freeaddrinfo(res);
    if (!n) {
        fprintf(stderr, "bnet: no addresses for '%s'\n", host);
        return 1;
    }
    return 0;
}

/* ---------- sockets ---------- */

typedef struct {
    char proto[8];
    char local[64];
    int port;
    int state;
    unsigned long inode;
} sockent_t;

static const char *tcp_state(int st)
{
    static const char *names[] = {
        "ESTABLISHED", "SYN_SENT", "SYN_RECV", "FIN_WAIT1", "FIN_WAIT2",
        "TIME_WAIT", "CLOSE", "CLOSE_WAIT", "LAST_ACK", "LISTEN",
        "CLOSING"
    };
    if (st >= 0 && st < 11)
        return names[st];
    return "UNKNOWN";
}

static void parse_proc_net(const char *path, const char *proto,
                           sockent_t *tab, int *n)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    char line[512];
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        return;
    }
    while (fgets(line, sizeof(line), f) && *n < 256) {
        char local[64] = "", rem[64] = "";
        unsigned int st = 0;
        unsigned long inode = 0;
        int uid = 0;
        if (sscanf(line, "%*d: %63s %63s %x %*x:%*x %*x:%*x %*x %d %*u %lu",
                   local, rem, &st, &uid, &inode) < 4)
            continue;
        sockent_t *e = &tab[*n];
        snprintf(e->proto, sizeof(e->proto), "%s", proto);
        e->state = (int)st;
        e->inode = inode;
        /* local is hex ip:port */
        char *colon = strrchr(local, ':');
        if (!colon)
            continue;
        *colon = 0;
        unsigned int iphex = 0, port = 0;
        if (sscanf(local, "%x", &iphex) == 1 &&
            sscanf(colon + 1, "%x", &port) == 1) {
            if (strstr(path, "6")) {
                /* 32 hex chars; IPv4-mapped shown as ::ffff:a.b.c.d */
                char ip6[64];
                snprintf(ip6, sizeof(ip6), "%s", local);
                struct in6_addr a6;
                if (inet_pton(AF_INET6, ip6, &a6) == 1) {
                    inet_ntop(AF_INET6, &a6, e->local, sizeof(e->local));
                } else {
                    snprintf(e->local, sizeof(e->local), "%s", ip6);
                }
                e->port = (int)port;
            } else {
                struct in_addr a = { .s_addr = iphex };
                inet_ntop(AF_INET, &a, e->local, sizeof(e->local));
                e->port = (int)port;
            }
            (*n)++;
        }
    }
    fclose(f);
}

static int find_proc_for_inode(unsigned long inode, char *comm, size_t csz,
                               int *pid_out)
{
    DIR *d = opendir("/proc");
    if (!d)
        return 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9')
            continue;
        char fddir[64];
        snprintf(fddir, sizeof(fddir), "/proc/%.*s/fd", 10,
                 e->d_name);
        DIR *fd = opendir(fddir);
        if (!fd)
            continue;
        struct dirent *fe;
        int found = 0;
        while ((fe = readdir(fd))) {
            char link[512];
            snprintf(link, sizeof(link), "%.*s/%.*s",
                     (int)sizeof(fddir) - 1, fddir,
                     (int)(sizeof(link) - sizeof(fddir) - 2), fe->d_name);
            char target[256];
            ssize_t n = readlink(link, target, sizeof(target) - 1);
            if (n > 0) {
                target[n] = 0;
                unsigned long ino = 0;
                if (sscanf(target, "socket:[%lu]", &ino) == 1 &&
                    ino == inode) {
                    found = 1;
                    break;
                }
            }
        }
        closedir(fd);
        if (found) {
            char cp[64];
            snprintf(cp, sizeof(cp), "/proc/%.*s/comm", 10,
                     e->d_name);
            FILE *f = fopen(cp, "r");
            if (f) {
                if (!fgets(comm, (int)csz, f))
                    snprintf(comm, csz, "?");
                fclose(f);
                comm[strcspn(comm, "\n")] = 0;
            } else {
                snprintf(comm, csz, "?");
            }
            if (pid_out)
                *pid_out = atoi(e->d_name);
            closedir(d);
            return 1;
        }
    }
    closedir(d);
    return 0;
}

static int cmd_sockets(void)
{
    sockent_t tab[256];
    int n = 0;
    parse_proc_net("/proc/net/tcp", "TCP", tab, &n);
    parse_proc_net("/proc/net/tcp6", "TCP6", tab, &n);
    parse_proc_net("/proc/net/udp", "UDP", tab, &n);
    parse_proc_net("/proc/net/udp6", "UDP6", tab, &n);

    printf("%-6s %-12s %-22s %-6s %s\n",
           "PROTO", "STATE", "ADDRESS:PORT", "PID", "PROCESS");
    for (int i = 0; i < n; i++) {
        const sockent_t *e = &tab[i];
        if (e->state != 10 /* LISTEN */ &&
            !(strncmp(e->proto, "UDP", 3) == 0 && e->state == 7))
            continue;
        char comm[64] = "-";
        int pid = 0;
        if (e->inode)
            find_proc_for_inode(e->inode, comm, sizeof(comm), &pid);
        printf("%-6s %-12s %s:%d %-6d %s\n", e->proto,
               strcmp(e->proto, "UDP") == 0 ? "LISTEN" : tcp_state(e->state),
               e->local, e->port, pid, comm);
    }
    return 0;
}

/* ---------- status / apply ---------- */

static int cmd_status(void)
{
    scan_addrs();
    DIR *d = opendir("/sys/class/net");
    if (!d) {
        perror("bnet: cannot open /sys/class/net");
        return 1;
    }
    struct dirent *e;
    int any_up = 0;
    printf("Interfaces:\n");
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        int flags = if_flags(e->d_name);
        int up = flags >= 0 && (flags & IFF_UP);
        if (up && strcmp(e->d_name, "lo") != 0)
            any_up = 1;
        const v4ent_t *v4 = iface_v4(e->d_name);
        char a[64] = "-";
        if (v4)
            inet_ntop(AF_INET, &v4->addr, a, sizeof(a));
        printf("  %-8s %-4s %s\n", e->d_name, up ? "UP" : "DOWN", a);
    }
    closedir(d);
    const char *gw = gateway4();
    printf("Gateway: %s\n", gw ? gw : "(none)");
    printf("DNS:\n");
    FILE *f = fopen(RESOLV, "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = 0;
            if (strncmp(line, "nameserver", 10) == 0)
                printf("  %s\n", line);
        }
        fclose(f);
    }
    return any_up ? 0 : 1;
}

/* ---------- network.conf ---------- */

typedef struct {
    char iface[MAX_IFNAME + 1];
    char method[16];
    char address[64];
    int prefix;
    char gateway[64];
    char dns[MAX_DNS][64];
    int ndns;
} netblock_t;

static netblock_t blocks[8];
static int nblocks;

static int parse_netconf(const char *path, char *err, size_t ersz)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        snprintf(err, ersz, "cannot open %s: %s", path, strerror(errno));
        return -1;
    }
    char line[256];
    int lineno = 0;
    int cur = -1;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        line[strcspn(line, "\r\n")] = 0;
        char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == 0 || *p == '#')
            continue;
        char *eq = strchr(p, '=');
        if (!eq) {
            snprintf(err, ersz, "%s:%d: expected key=value", path, lineno);
            fclose(f);
            return -1;
        }
        *eq = 0;
        char *key = p;
        char *val = eq + 1;
        while (key[0] == ' ' || key[0] == '\t')
            key++;
        size_t kl = strlen(key);
        while (kl && (key[kl - 1] == ' ' || key[kl - 1] == '\t'))
            key[--kl] = 0;
        if (strcmp(key, "interface") == 0) {
            if (nblocks >= 8) {
                snprintf(err, ersz, "%s:%d: too many interfaces", path,
                         lineno);
                fclose(f);
                return -1;
            }
            cur = nblocks++;
            memset(&blocks[cur], 0, sizeof(blocks[cur]));
            snprintf(blocks[cur].iface, sizeof(blocks[cur].iface), "%s", val);
            snprintf(blocks[cur].method, sizeof(blocks[cur].method), "dhcp");
            continue;
        }
        if (cur < 0) {
            snprintf(err, ersz, "%s:%d: '%s' before any interface= line",
                     path, lineno, key);
            fclose(f);
            return -1;
        }
        netblock_t *b = &blocks[cur];
        if (strcmp(key, "method") == 0) {
            snprintf(b->method, sizeof(b->method), "%s", val);
        } else if (strcmp(key, "address") == 0) {
            snprintf(b->address, sizeof(b->address), "%s", val);
        } else if (strcmp(key, "gateway") == 0) {
            snprintf(b->gateway, sizeof(b->gateway), "%s", val);
        } else if (strcmp(key, "dns") == 0) {
            if (b->ndns < MAX_DNS)
                snprintf(b->dns[b->ndns++], 64, "%s", val);
        } else {
            snprintf(err, ersz, "%s:%d: unknown key '%s'", path, lineno,
                     key);
            fclose(f);
            return -1;
        }
    }
    fclose(f);
    return 0;
}

static int apply_block(const netblock_t *b)
{
    if (!valid_ifname(b->iface) || !iface_exists(b->iface)) {
        fprintf(stderr, "bnet: interface '%s' does not exist\n", b->iface);
        return 1;
    }
    if (strcmp(b->method, "dhcp") == 0) {
        set_if_flag(b->iface, IFF_UP, 1);
        unlink(UDHCP_PIDFILE);
        int rc = run_udhcpc(b->iface, 10);
        scan_addrs();
        if (rc != 0 || !iface_v4(b->iface)) {
            fprintf(stderr, "bnet: DHCP on %s: no lease yet\n", b->iface);
            return 1;
        }
        const v4ent_t *v4 = iface_v4(b->iface);
        char a[64];
        inet_ntop(AF_INET, &v4->addr, a, sizeof(a));
        printf("bnet: %s: DHCP %s\n", b->iface, a);
        return 0;
    }
    if (strcmp(b->method, "static") == 0) {
        if (!b->address[0]) {
            fprintf(stderr, "bnet: static method needs address= for %s\n",
                    b->iface);
            return 1;
        }
        if (cmd_ip_add(b->address, b->iface) != 0)
            return 1;
        if (b->gateway[0]) {
            char *av[2];
            av[0] = (char *)b->gateway;
            av[1] = NULL;
            if (cmd_route_add_del(0, 2, av) != 0)
                return 1;
        }
        if (b->ndns > 0) {
            char *av[MAX_DNS];
            for (int i = 0; i < b->ndns; i++)
                av[i] = (char *)b->dns[i];
            if (cmd_dns_set(b->ndns, av) != 0)
                return 1;
        }
        printf("bnet: %s: static %s\n", b->iface, b->address);
        return 0;
    }
    fprintf(stderr, "bnet: unknown method '%s' for %s\n", b->method,
            b->iface);
    return 1;
}

static int cmd_apply(void)
{
    const char *conf = getenv("BNET_CONF");
    if (!conf || !conf[0])
        conf = NETCONF;
    char err[256];
    if (parse_netconf(conf, err, sizeof(err)) != 0) {
        fprintf(stderr, "bnet: %s\n", err);
        return 1;
    }
    if (nblocks == 0) {
        fprintf(stderr, "bnet: no interface= entries in %s\n", conf);
        return 1;
    }
    int bad = 0;
    for (int i = 0; i < nblocks; i++)
        if (apply_block(&blocks[i]) != 0)
            bad++;
    return bad ? 1 : 0;
}


/* ---------- WiFi (Phase 9.5) ---------- */

#define WPA_CONF "/etc/wpa_supplicant.conf"
#define WPA_BIN  "/usr/sbin/wpa_supplicant"
#define WPA_CLI  "/usr/sbin/wpa_cli"
#define WPA_PID  "/run/wpa_supplicant.pid"

static const char *wpa_conf_path(void);

static int file_exists(const char *p)
{
    return access(p, F_OK) == 0;
}

static int wpa_available(void)
{
    return access(WPA_BIN, X_OK) == 0 && access(WPA_CLI, X_OK) == 0;
}

static int find_wlan(char *out, size_t n)
{
    const char *ov = getenv("BNET_WLAN");
    if (ov && ov[0]) {
        snprintf(out, n, "%s", ov);
        return 0;
    }
    DIR *d = opendir("/sys/class/net");
    if (!d) return -1;
    struct dirent *e;
    int found = -1;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char p[512];
        snprintf(p, sizeof(p), "/sys/class/net/%s/wireless", e->d_name);
        if (file_exists(p)) {
            strncpy(out, e->d_name, n - 1);
            out[n - 1] = '\0';
            found = 0;
            break;
        }
    }
    closedir(d);
    return found;
}

static int wpa_running(void)
{
    return file_exists(WPA_PID) || access("/var/run/wpa_supplicant", F_OK) == 0;
}

static int cmd_wifi_status(void)
{
    char wlan[IFNAMSIZ];
    printf("WiFi status:\n");
    if (find_wlan(wlan, sizeof(wlan)) == 0)
        printf("  interface        %s\n", wlan);
    else
        printf("  interface        (none - no wireless adapter detected)\n");
    printf("  wpa_supplicant   %s\n",
           wpa_available() ? "available" : "NOT installed");
    printf("  wpa_supplicant   %s\n",
           wpa_running() ? "running" : "not running");
    printf("  config           %s\n", wpa_conf_path());
    return 0;
}

static const char *wpa_conf_path(void)
{
    const char *c = getenv("BNET_WPA_CONF");
    return (c && c[0]) ? c : WPA_CONF;
}

static int write_wpa_conf(const char *ssid, const char *psk)
{
    const char *conf = wpa_conf_path();
    char tmp[160];
    snprintf(tmp, sizeof(tmp), "%s.tmp.%d", conf, (int)getpid());
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        fprintf(stderr, "bnet: cannot write %s: %s\n", tmp, strerror(errno));
        return 1;
    }
    dprintf(fd, "ctrl_interface=/var/run/wpa_supplicant\n");
    dprintf(fd, "update_config=1\n");
    dprintf(fd, "country=US\n");
    dprintf(fd, "network={\n");
    dprintf(fd, "    ssid=\"%s\"\n", ssid);
    if (psk && psk[0])
        dprintf(fd, "    psk=\"%s\"\n", psk);
    else
        dprintf(fd, "    key_mgmt=NONE\n");   /* open network */
    dprintf(fd, "}\n");
    close(fd);
    if (rename(tmp, conf) < 0) {
        unlink(tmp);
        fprintf(stderr, "bnet: cannot update %s: %s\n", conf,
                strerror(errno));
        return 1;
    }
    return 0;
}

static int cmd_wifi_connect(int argc, char **argv)
{
    const char *ssid = argv[0];
    const char *psk = (argc >= 2) ? argv[1] : "";
    char wlan[IFNAMSIZ];
    if (find_wlan(wlan, sizeof(wlan)) != 0) {
        fprintf(stderr, "bnet: no wireless interface found\n");
        return 1;
    }
    if (write_wpa_conf(ssid, psk) != 0)
        return 1;
    if (wpa_available()) {
        printf("bnet: wrote %s; run: %s -B -i%s -c%s "
               "then %s -i%s add_network/select_network (or a DHCP client)\n",
               wpa_conf_path(), WPA_BIN, wlan, wpa_conf_path(), WPA_CLI, wlan);
        /* best-effort start via exec (won't block on success) */
        return 0;
    }
    printf("bnet: %s not installed; wrote %s "
           "for later use (install wpa_supplicant to activate)\n",
           WPA_BIN, wpa_conf_path());
    return 0;
}

static int cmd_wifi_scan(void)
{
    char wlan[IFNAMSIZ];
    if (find_wlan(wlan, sizeof(wlan)) != 0) {
        fprintf(stderr, "bnet: no wireless interface found\n");
        return 1;
    }
    if (!wpa_available()) {
        fprintf(stderr, "bnet: %s not installed; cannot scan\n", WPA_BIN);
        return 1;
    }
    if (!wpa_running()) {
        fprintf(stderr, "bnet: wpa_supplicant not running; start it first\n");
        return 1;
    }
    /* scan needs a shell pipeline; fall back to wpa_cli directly */
    printf("bnet: scan may need a running wpa_supplicant; "
           "use: %s -i%s scan && %s -i%s scan_results\n",
           WPA_CLI, wlan, WPA_CLI, wlan);
    return 0;
}

static int cmd_wifi_disconnect(void)
{
    char wlan[IFNAMSIZ];
    if (find_wlan(wlan, sizeof(wlan)) != 0) {
        fprintf(stderr, "bnet: no wireless interface found\n");
        return 1;
    }
    if (wpa_available() && file_exists(WPA_PID)) {
        printf("bnet: stop wpa_supplicant via: kill $(cat %s)\n", WPA_PID);
        return 0;
    }
    printf("bnet: wpa_supplicant not running\n");
    return 0;
}

static int cmd_wifi(int argc, char **argv)
{
    if (argc == 0)
        return cmd_wifi_status();
    if (!strcmp(argv[0], "status"))
        return cmd_wifi_status();
    if (!strcmp(argv[0], "scan") || !strcmp(argv[0], "list"))
        return cmd_wifi_scan();
    if (!strcmp(argv[0], "connect"))
        return argc >= 2 ? cmd_wifi_connect(argc - 1, argv + 1)
                         : (fprintf(stderr, "usage: bnet wifi connect <ssid> [psk]\n"), 2);
    if (!strcmp(argv[0], "disconnect"))
        return cmd_wifi_disconnect();
    fprintf(stderr, "usage: bnet wifi [status|scan|connect <ssid> [psk]|disconnect]\n");
    return 2;
}

/* ---------- main ---------- */

static void usage(const char *p)
{
    printf("usage: %s <command> [args]\n"
           "  status                     overall network summary\n"
           "  list                       interface names\n"
           "  info <iface>               interface details\n"
           "  up <iface> | down <iface>  bring interface up/down\n"
           "  ip <iface>                 show addresses\n"
           "  ip add <addr/prefix> <if>  assign IPv4 address\n"
           "  ip del <addr/prefix> <if>  remove IPv4 address\n"
           "  route                      show routing table\n"
           "  route add <dest> <gw> [iface]\n"
           "  route del <dest> <gw> [iface]\n"
           "  dns                        show DNS servers\n"
           "  dns set <ns>...            set /etc/resolv.conf\n"
           "  dhcp <iface>               acquire lease via udhcpc\n"
           "  ping <host>                ping host (3 packets)\n"
           "  resolve <host>             resolve hostname\n"
           "  sockets                    listening sockets\n"
           "  wifi                       WiFi status\n"
           "  wifi scan                  scan for networks\n"
           "  wifi connect <ssid> [psk]  join a network (writes conf)\n"
           "  wifi disconnect            stop wpa_supplicant\n"
           "  apply                      apply " NETCONF "\n"
           "  --help | --version\n", p);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage(argv[0]);
        return 2;
    }
    const char *cmd = argv[1];
    if (!strcmp(cmd, "--help") || !strcmp(cmd, "-h")) {
        usage(argv[0]);
        return 0;
    }
    if (!strcmp(cmd, "--version")) {
        printf("bnet %s (BulumOS)\n", BNET_VERSION);
        return 0;
    }
    int rc;
    if (!strcmp(cmd, "status"))
        rc = cmd_status();
    else if (!strcmp(cmd, "list"))
        rc = cmd_list();
    else if (!strcmp(cmd, "info"))
        rc = argc >= 3 ? cmd_info(argv[2]) : (usage(argv[0]), 2);
    else if (!strcmp(cmd, "up"))
        rc = argc >= 3 ? cmd_up(argv[2]) : (usage(argv[0]), 2);
    else if (!strcmp(cmd, "down"))
        rc = argc >= 3 ? cmd_down(argv[2]) : (usage(argv[0]), 2);
    else if (!strcmp(cmd, "ip")) {
        if (argc >= 4 && !strcmp(argv[2], "add"))
            rc = cmd_ip_add(argv[3], argv[4]);
        else if (argc >= 4 && !strcmp(argv[2], "del"))
            rc = cmd_ip_del(argv[3], argv[4]);
        else if (argc >= 3)
            rc = cmd_ip(argv[2]);
        else {
            usage(argv[0]);
            rc = 2;
        }
    } else if (!strcmp(cmd, "route")) {
        if (argc >= 3 && !strcmp(argv[2], "add"))
            rc = cmd_route_add_del(0, argc - 3, argv + 3);
        else if (argc >= 3 && !strcmp(argv[2], "del"))
            rc = cmd_route_add_del(1, argc - 3, argv + 3);
        else
            rc = cmd_route();
    } else if (!strcmp(cmd, "dns")) {
        if (argc >= 3 && !strcmp(argv[2], "set"))
            rc = cmd_dns_set(argc - 3, argv + 3);
        else
            rc = cmd_dns();
    } else if (!strcmp(cmd, "dhcp"))
        rc = argc >= 3 ? cmd_dhcp(argv[2]) : (usage(argv[0]), 2);
    else if (!strcmp(cmd, "ping"))
        rc = cmd_ping(argc - 2, argv + 2);
    else if (!strcmp(cmd, "resolve"))
        rc = argc >= 3 ? cmd_resolve(argv[2]) : (usage(argv[0]), 2);
    else if (!strcmp(cmd, "sockets"))
        rc = cmd_sockets();
    else if (!strcmp(cmd, "wifi"))
        rc = cmd_wifi(argc - 2, argv + 2);
    else if (!strcmp(cmd, "apply"))
        rc = cmd_apply();
    else {
        usage(argv[0]);
        rc = 2;
    }
    return rc;
}