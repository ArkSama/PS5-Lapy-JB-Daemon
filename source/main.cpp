/* ============================================================================
 * lapy_jb_daemon v1.2 — multi-firmware standalone homebrew daemon
 *
 * Mimics etaHEN's jailbreak-on-demand daemon API for Sony app PRXes that
 * write /download0/etahen_jailbreak from inside their sandbox.
 *
 * v1.2 = revert to libhijacker-based jailbreak (= proven, multi-fw via
 * ps5-payload-sdk runtime KERNEL_ADDRESS_* resolution). v1.1 had a manual
 * reimpl with bugs (wrong authid, wrong cr_sceAttr offset, missing sandbox
 * escape via fd_rdir/jdir → rootvnode). Reverting fixes all that.
 *
 * Multi-fw mechanism :
 *   - ps5-payload-sdk (libkernel_sys) auto-detects fw at boot via
 *     payload_args->kdata_base_addr and populates KERNEL_ADDRESS_* extern
 *     globals (allproc, rootvnode, etc.) with the correct per-fw values.
 *   - The offsets_v940.cpp override (linked before -lhijacker) converts
 *     KERNEL_ADDRESS_* → relative offsets. Multi-fw transparent.
 *   - Compatible : every fw covered by ps5-payload-sdk's runtime tables
 *     (3.00 → 12.00 via kstuff/kstuff-lite + ps5-hen).
 *
 * Built with ps5-payload-sdk (John Törnblom).
 * Credits : libhijacker (illusion + LightingMod), modified offsets by
 *           Team PHU / Arksama.
 * ============================================================================ */

#include <hijacker.hpp>
#include <kernel/proc.hpp>
#include <util.hpp>

extern "C" {
#include <dirent.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mount.h>
#include <unistd.h>
#include <ps5/payload.h>
#include <ps5/kernel.h>

int klog_printf(const char *fmt, ...);
int sceNotificationSend(int userId, bool isLogged, const char *payload);
}

/* libhijacker references kernel_base as extern "C" — the definition lives
 * here as the standalone-payload entry point. Set from payload_args. */
extern "C" uint64_t kernel_base = 0;

#define LOG(fmt, ...) klog_printf("[lapy_jb] " fmt "\n", ##__VA_ARGS__)

#define SANDBOX_BASE      "/mnt/sandbox"
#define JB_FILE_RELPATH   "/download0/etahen_jailbreak"
#define POLL_INTERVAL_US  (250 * 1000)

/* Modern Sony notification — kstuff-INDEPENDENT, libSceNotification IPC. */
static void notif(const char *fmt, ...) {
    char text[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    char json[1024];
    snprintf(json, sizeof(json),
        "{\"rawData\":{"
        "\"viewTemplateType\":\"InteractiveToastTemplateB\","
        "\"channelType\":\"Downloads\","
        "\"useCaseId\":\"IDC\","
        "\"toastOverwriteType\":\"Yes\","
        "\"isImmediate\":true,"
        "\"priority\":100,"
        "\"viewData\":{"
        "\"icon\":{\"type\":\"Predefined\",\"parameters\":{\"icon\":\"download\"}},"
        "\"message\":{\"body\":\"%s\"},"
        "\"subMessage\":{\"body\":\"Lapy JB Daemon\"}"
        "},"
        "\"platformViews\":{"
        "\"previewDisabled\":{"
        "\"viewData\":{"
        "\"icon\":{\"type\":\"Predefined\",\"parameters\":{\"icon\":\"download\"}},"
        "\"message\":{\"body\":\"%s\"}"
        "}"
        "}"
        "}"
        "},"
        "\"localNotificationId\":\"LAPY_JB\""
        "}",
        text, text);
    sceNotificationSend(0xFE, true, json);
}

/* ----- File helpers ------------------------------------------------------- */
static ssize_t slurp_file(const char *path, char *buf, size_t buf_size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, buf_size - 1);
    close(fd);
    if (n < 0) return -1;
    buf[n] = '\0';
    return n;
}

/* Parse {"PID":"<int>"} JSON. Returns pid or -1. */
static pid_t parse_pid_from_json(const char *json) {
    const char *p = strstr(json, "\"PID\"");
    if (!p) return -1;
    p = strchr(p, ':');
    if (!p) return -1;
    p++;
    while (*p && (*p == ' ' || *p == '\t' || *p == '"')) p++;
    if (!*p) return -1;
    pid_t pid = (pid_t)atoi(p);
    return pid > 0 ? pid : -1;
}

/* Bump target's ucred via libhijacker (= jailbreak() : caps + authid + uid +
 * sceAttr + sandbox escape via fd_rdir/jdir → rootvnode). Returns 0 on success. */
static int do_jailbreak(pid_t target_pid) {
    LOG("jailbreak attempt : pid=%d", target_pid);
    auto hj = Hijacker::getHijacker(target_pid);
    if (!hj) {
        LOG("getHijacker(%d) FAIL — process likely exited", target_pid);
        return -1;
    }
    /* libhijacker's jailbreak() does ALL of :
     *   - cr_uid / cr_ruid / cr_svuid = 0
     *   - cr_ngroups = 0, cr_rgid = 0
     *   - cr_sceCaps[0..1] = 0xFFFFFFFFFFFFFFFF
     *   - cr_sceAuthID = 0x4801000000000013 (jailbreak authid)
     *   - cr_sceAttr[0] = 0x80 at offset 0x83 (= unsandboxed)
     *   - fd_rdir / fd_jdir = rootvnode (= sandbox escape)
     * Multi-fw via ps5-payload-sdk's runtime KERNEL_ADDRESS_*. */
    hj->jailbreak(/*escapeSandbox=*/ true);
    LOG("jailbreak OK pid=%d", target_pid);
    return 0;
}

/* ----- Update blocker (port of etaHEN bootstrapper main.cpp) -------------
 * A downloaded PS5UPDATE.PUP in /update makes the system demand an update
 * before it mounts extended storage. Delete it and unmount /update. */
static void block_updates(void) {
    unlink("/update/PS5UPDATE.PUP");
    unlink("/update/PS5UPDATE.PUP.net.temp");
    if (unmount("/update", 0x80000) < 0)
        unmount("/update", 0);
    LOG("update blocker: /update cleaned and unmounted");
}

/* ----- Main loop ---------------------------------------------------------- */
int main(void) {
    LOG("======================================");
    LOG("  Lapy JB Daemon v1.2 (multi-fw)");
    LOG("  No etaHEN, no PHU required");
    LOG("======================================");

    payload_args_t *args = payload_get_args();
    if (!args) {
        LOG("FATAL : payload_get_args() returned NULL — must run via elfldr/HBL");
        notif("Lapy JB FATAL : needs elfldr launcher");
        return 1;
    }
    kernel_base = args->kdata_base_addr;
    LOG("kernel_base = 0x%lx", (uintptr_t)kernel_base);

    block_updates();

    notif("Lapy JB Daemon active — launch your app");
    LOG("polling %s/<TID>_<NNN>%s every %dms",
        SANDBOX_BASE, JB_FILE_RELPATH, POLL_INTERVAL_US / 1000);

    char buf[512];
    char path[512];
    while (true) {
        DIR *d = opendir(SANDBOX_BASE);
        if (!d) { usleep(POLL_INTERVAL_US); continue; }
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (de->d_name[0] == '.') continue;
            int n = snprintf(path, sizeof(path), "%s/%s%s",
                             SANDBOX_BASE, de->d_name, JB_FILE_RELPATH);
            if (n <= 0 || (size_t)n >= sizeof(path)) continue;

            struct stat st;
            if (stat(path, &st) != 0 || st.st_size == 0) continue;

            ssize_t got = slurp_file(path, buf, sizeof(buf));
            if (got > 0) {
                pid_t pid = parse_pid_from_json(buf);
                if (pid > 0) {
                    LOG("request from %s : pid=%d", de->d_name, pid);
                    if (do_jailbreak(pid) == 0) {
                        notif("%s pid=%d unsandboxed", de->d_name, (int)pid);
                    } else {
                        notif("Jailbreak FAIL pid=%d", (int)pid);
                    }
                } else {
                    LOG("could not parse PID from '%s' (%s)", buf, path);
                }
            }
            unlink(path);   /* signal to PRX : bump done */
        }
        closedir(d);
        usleep(POLL_INTERVAL_US);
    }
    return 0;
}
