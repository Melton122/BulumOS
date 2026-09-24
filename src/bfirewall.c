/*
 * bfirewall - BulumOS firewall interface (Phase 7)
 *
 * Reports what the running kernel actually provides and manages a
 * conservative allowlist rule set when iptables/nft userspace exists.
 *
 * On BulumOS today the kernel has netfilter built in but no iptables/nft
 * userspace and no loadable modules (no /lib/modules), so enable/disable
 * report the situation honestly instead of pretending to filter.
 *
 *   bfirewall status     kernel + tooling availability
 *   bfirewall list       current rules (if tooling exists)
 *   bfirewall enable     apply conservative allowlist (if possible)
 *   bfirewall disable    flush rules (if possible)
 *
 * No system(), no popen(); external tools are execve'd with fixed paths.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define BFIREWALL_VERSION "1.0.0"

static int file_exists(const char *p)
{
    struct stat s;
    return stat(p, &s) == 0;
}

/* does /proc/net report a loaded ip_tables table? */
static int iptables_loaded(void)
{
    if (file_exists("/proc/net/ip_tables_names"))
        return 1;
    /* table names can appear even with an empty ruleset */
    return 0;
}

static int nft_loaded(void)
{
    return file_exists("/proc/net/netfilter/nf_tables");
}

/* fixed paths to look for */
static const char *tool_paths[] = {
    "/sbin/iptables", "/usr/sbin/iptables",
    "/usr/sbin/nft", "/sbin/nft", NULL
};

static const char *find_tool(void)
{
    for (int i = 0; tool_paths[i]; i++)
        if (file_exists(tool_paths[i]))
            return tool_paths[i];
    return NULL;
}

static int netfilter_present(void)
{
    return file_exists("/proc/net/netfilter") || iptables_loaded();
}

static int cmd_status(void)
{
    printf("Firewall status\n");
    if (netfilter_present())
        printf("  Kernel netfilter:  present\n");
    else
        printf("  Kernel netfilter:  not detected\n");
    printf("  iptables tables:    %s\n",
           iptables_loaded() ? "loaded" : "not loaded");
    printf("  nftables:           %s\n",
           nft_loaded() ? "present" : "not present");
    const char *tool = find_tool();
    printf("  iptables/nft tool:  %s\n",
           tool ? tool : "not installed");
    if (!tool)
        printf("  verdict: firewall unavailable; no rules can be applied.\n"
               "           kernel netfilter is built in (modules not\n"
               "           loadable without /lib/modules).\n");
    else
        printf("  verdict: firewall tooling available\n");
    return tool ? 0 : 1;
}

static int run_tool(const char *tool, char *const args[])
{
    pid_t pid = fork();
    if (pid < 0)
        return 1;
    if (pid == 0) {
        execv(tool, args);
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return 1;
}

static int cmd_list(void)
{
    const char *tool = find_tool();
    if (!tool) {
        fprintf(stderr,
                "bfirewall: no iptables/nft tool installed; nothing to list\n");
        return 1;
    }
    if (strstr(tool, "nft")) {
        char *args[] = { (char *)tool, "list", "ruleset", NULL };
        return run_tool(tool, args);
    }
    char *args[] = { (char *)tool, "-L", "-n", "-v", NULL };
    return run_tool(tool, args);
}

/* conservative allowlist: loopback, established, ssh/http/dns/dhcp/icmp,
 * outbound all; nothing drops QEMU slirp traffic or package downloads */
static const char *rules[] = {
    "-P INPUT DROP",
    "-P FORWARD DROP",
    "-P OUTPUT ACCEPT",
    "-A INPUT -i lo -j ACCEPT",
    "-A INPUT -m state --state ESTABLISHED,RELATED -j ACCEPT",
    "-A INPUT -p tcp --dport 22 -j ACCEPT",
    "-A INPUT -p tcp --dport 80 -j ACCEPT",
    "-A INPUT -p udp --dport 53 -j ACCEPT",
    "-A INPUT -p tcp --dport 53 -j ACCEPT",
    "-A INPUT -p udp --dport 67:68 -j ACCEPT",
    "-A INPUT -p icmp --icmp-type echo-request -j ACCEPT",
    "-A INPUT -j REJECT --reject-with icmp-port-unreachable",
    NULL
};

static int cmd_enable(void)
{
    const char *tool = find_tool();
    if (!tool || strstr(tool, "nft")) {
        fprintf(stderr,
                "bfirewall: no iptables tool installed; cannot enable.\n"
                "  Install iptables userspace and ensure the ip_tables\n"
                "  module is loadable, then retry.\n");
        return 1;
    }
    int rc = 0;
    for (int i = 0; rules[i]; i++) {
        char *args[8];
        int n = 0;
        args[n++] = (char *)tool;
        char tmp[128];
        snprintf(tmp, sizeof(tmp), "%s", rules[i]);
        char *save = NULL;
        for (char *tok = strtok_r(tmp, " ", &save); tok && n < 7;
             tok = strtok_r(NULL, " ", &save))
            args[n++] = tok;
        args[n] = NULL;
        if (run_tool(tool, args) != 0)
            rc = 1;
    }
    if (rc == 0)
        printf("bfirewall: conservative allowlist applied\n");
    return rc;
}

static int cmd_disable(void)
{
    const char *tool = find_tool();
    if (!tool || strstr(tool, "nft")) {
        fprintf(stderr, "bfirewall: no iptables tool installed\n");
        return 1;
    }
    char *args[] = { (char *)tool, "-F", NULL };
    run_tool(tool, args);
    char *args2[] = { (char *)tool, "-P", "INPUT", "ACCEPT", NULL };
    run_tool(tool, args2);
    char *args3[] = { (char *)tool, "-P", "FORWARD", "ACCEPT", NULL };
    run_tool(tool, args3);
    printf("bfirewall: rules flushed, policy ACCEPT\n");
    return 0;
}

static void usage(const char *p)
{
    printf("usage: %s <command>\n"
           "  status          firewall availability\n"
           "  list            current rules\n"
           "  enable          apply conservative allowlist\n"
           "  disable         flush rules\n"
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
        printf("bfirewall %s (BulumOS)\n", BFIREWALL_VERSION);
        return 0;
    }
    if (!strcmp(cmd, "status"))
        return cmd_status();
    if (!strcmp(cmd, "list"))
        return cmd_list();
    if (!strcmp(cmd, "enable"))
        return cmd_enable();
    if (!strcmp(cmd, "disable"))
        return cmd_disable();
    usage(argv[0]);
    return 2;
}