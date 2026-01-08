#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <stdarg.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/uio.h>
#include <errno.h>
#include <stdbool.h>
#include <time.h>
#include <sys/syscall.h>

#include <ps5/kernel.h> 

// --- Configuration ---
#define SCAN_INTERVAL_US    3000000 
#define STABLE_WAIT_US      3000000 
#define MAX_PENDING         512     
#define MAX_PATH            1024
#define MAX_TITLE_ID        32
#define MAX_TITLE_NAME      256
#define LOG_DIR             "/data/shadowmount"
#define LOG_FILE            "/data/shadowmount/debug.log"
#define LOCK_FILE           "/data/shadowmount/daemon.lock"
#define KILL_FILE           "/data/shadowmount/STOP"
#define TOAST_FILE          "/data/shadowmount/notify.txt"
#define IOVEC_ENTRY(x) { (void*)(x), (x) ? strlen(x) + 1 : 0 }
#define IOVEC_SIZE(x)  (sizeof(x) / sizeof(struct iovec))

// --- SDK Imports ---
int sceAppInstUtilInitialize(void);
int sceAppInstUtilAppInstallTitleDir(const char* title_id, const char* install_path, void* reserved);
int sceKernelUsleep(unsigned int microseconds);
int sceUserServiceInitialize(void*);
void sceUserServiceTerminate(void);

// --- Forward Declarations ---
bool get_game_info(const char* base_path, char* out_id, char* out_name);
bool is_installed(const char* title_id);
bool is_data_mounted(const char* title_id);
void notify_system(const char* fmt, ...);
void log_debug(const char* fmt, ...);

// Standard Notification
typedef struct notify_request { char unused[45]; char message[3075]; } notify_request_t;
int sceKernelSendNotificationRequest(int, notify_request_t*, size_t, int);

const char* SCAN_PATHS[] = {
    "/data/homebrew", 
    "/mnt/usb0/homebrew", "/mnt/usb1/homebrew", "/mnt/usb2/homebrew",
    "/mnt/usb3/homebrew", "/mnt/usb4/homebrew", "/mnt/usb5/homebrew", "/mnt/usb6/homebrew",
    "/mnt/ext0/homebrew", "/mnt/ext1/homebrew",
    "/data/etaHEN/games",
    "/mnt/usb0/etaHEN/games", "/mnt/usb1/etaHEN/games", "/mnt/usb2/etaHEN/games",
    "/mnt/usb3/etaHEN/games", "/mnt/usb4/etaHEN/games", "/mnt/usb5/etaHEN/games", "/mnt/usb6/etaHEN/games",
    "/mnt/ext0/etaHEN/games", "/mnt/ext1/etaHEN/games",
    NULL
};

struct GameCache { 
    char path[MAX_PATH]; 
    char title_id[MAX_TITLE_ID]; 
    char title_name[MAX_TITLE_NAME]; 
    bool valid; 
};
struct GameCache cache[MAX_PENDING];

__attribute__((constructor)) static void sys_init(void) {
    sceUserServiceInitialize(0);
    kernel_set_ucred_authid(-1, 0x4801000000000013L);
}
__attribute__((destructor)) static void sys_fini(void) { sceUserServiceTerminate(); }

// --- LOGGING ---
void log_to_file(const char* fmt, va_list args) {
    mkdir(LOG_DIR, 0777);
    FILE* fp = fopen(LOG_FILE, "a");
    if (fp) {
        time_t rawtime; struct tm * timeinfo; char buffer[80];
        time(&rawtime); timeinfo = localtime(&rawtime); strftime(buffer, sizeof(buffer), "%H:%M:%S", timeinfo);
        fprintf(fp, "[%s] ", buffer); vfprintf(fp, fmt, args); fprintf(fp, "\n"); fclose(fp);
    }
}
void log_debug(const char* fmt, ...) {
    va_list args; va_start(args, fmt); vprintf(fmt, args); printf("\n"); log_to_file(fmt, args); va_end(args);
}

// --- STANDARD NOTIFICATION ---
void notify_system(const char* fmt, ...) {
    notify_request_t req; memset(&req, 0, sizeof(req));
    va_list args; va_start(args, fmt); vsnprintf(req.message, sizeof(req.message), fmt, args); va_end(args);
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
    log_debug("NOTIFY: %s", req.message);
}

// --- RICH TOAST IPC ---
void trigger_rich_toast(const char* title_id, const char* game_name, const char* msg) {
    FILE* f = fopen(TOAST_FILE, "w");
    if (f) {
        fprintf(f, "%s|%s|%s", title_id, game_name, msg);
        fflush(f); 
        fclose(f);
        log_debug("IPC: Sent toast request for %s", game_name);
    }
}

// --- FILESYSTEM ---
bool is_installed(const char* title_id) { char path[MAX_PATH]; snprintf(path, sizeof(path), "/user/app/%s", title_id); struct stat st; return (stat(path, &st) == 0); }
bool is_data_mounted(const char* title_id) { char path[MAX_PATH]; snprintf(path, sizeof(path), "/system_ex/app/%s/sce_sys/param.json", title_id); return (access(path, F_OK) == 0); }

// --- RECURSION SAFETY ---
off_t get_folder_size_recursive(const char* path, int depth) {
    if (depth > 3) return 0; // Prevent stack overflow on massive games
    off_t total = 0; DIR* d = opendir(path); if (!d) return 0;
    struct dirent* entry; char full_path[MAX_PATH]; struct stat st;
    while ((entry = readdir(d)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);
        if (stat(full_path, &st) == 0) { 
            if (S_ISDIR(st.st_mode)) total += get_folder_size_recursive(full_path, depth + 1); 
            else total += st.st_size; 
        }
    }
    closedir(d); return total;
}

bool wait_for_stability(const char* path, const char* name) {
    log_debug("  [DEBUG] Checking stability for %s...", name); 
    off_t size1 = get_folder_size_recursive(path, 0);
    if (size1 == 0) return false;
    
    int checks = 0;
    while (true) {
        sceKernelUsleep(STABLE_WAIT_US); 
        off_t size2 = get_folder_size_recursive(path, 0);
        if (size1 == size2 && size1 > 0) {
            log_debug("  [DEBUG] Stable."); 
            return true;
        }
        log_debug("  [WAIT] Copying %s... (%ld -> %ld)", name, size1, size2);
        size1 = size2;
        checks++;
        if (checks > 100) return false; 
    }
}

static int remount_system_ex(void) {
    struct iovec iov[] = { IOVEC_ENTRY("from"), IOVEC_ENTRY("/dev/ssd0.system_ex"), IOVEC_ENTRY("fspath"), IOVEC_ENTRY("/system_ex"), IOVEC_ENTRY("fstype"), IOVEC_ENTRY("exfatfs"), IOVEC_ENTRY("large"), IOVEC_ENTRY("yes"), IOVEC_ENTRY("timezone"), IOVEC_ENTRY("static"), IOVEC_ENTRY("async"), IOVEC_ENTRY(NULL), IOVEC_ENTRY("ignoreacl"), IOVEC_ENTRY(NULL) };
    return nmount(iov, IOVEC_SIZE(iov), MNT_UPDATE);
}
static int mount_nullfs(const char* src, const char* dst) {
    struct iovec iov[] = { IOVEC_ENTRY("fstype"), IOVEC_ENTRY("nullfs"), IOVEC_ENTRY("from"), IOVEC_ENTRY(src), IOVEC_ENTRY("fspath"), IOVEC_ENTRY(dst) };
    return nmount(iov, IOVEC_SIZE(iov), MNT_RDONLY); 
}
static int copy_dir(const char* src, const char* dst) {
    mkdir(dst, 0777); DIR* d = opendir(src); if (!d) return -1;
    struct dirent* e; char ss[MAX_PATH], dd[MAX_PATH]; struct stat st;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(ss, sizeof(ss), "%s/%s", src, e->d_name); snprintf(dd, sizeof(dd), "%s/%s", dst, e->d_name);
        if (stat(ss, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) copy_dir(ss, dd);
        else {
            FILE* fs = fopen(ss, "rb"); if (!fs) continue;
            FILE* fd = fopen(dd, "wb"); if (!fd) { fclose(fs); continue; }
            char buf[8192]; size_t n; while ((n = fread(buf, 1, sizeof(buf), fs)) > 0) fwrite(buf, 1, n, fd);
            fclose(fd); fclose(fs);
        }
    }
    closedir(d); return 0;
}
int copy_file(const char* src, const char* dst) {
    char buf[8192]; FILE* fs = fopen(src, "rb"); if (!fs) return -1;
    FILE* fd = fopen(dst, "wb"); if (!fd) { fclose(fs); return -1; }
    size_t n; while ((n = fread(buf, 1, sizeof(buf), fs)) > 0) fwrite(buf, 1, n, fd);
    fclose(fd); fclose(fs); return 0;
}

// --- JSON & DRM ---
static int extract_json_string(const char* json, const char* key, char* out, size_t out_size) {
    char search[64]; snprintf(search, sizeof(search), "\"%s\"", key);
    const char* p = strstr(json, search); if (!p) return -1;
    p = strchr(p + strlen(search), ':'); if (!p) return -2;
    while (*++p && isspace(*p)) { /*skip*/ } if (*p != '"') return -3; p++;
    size_t i = 0; while (i < out_size - 1 && p[i] && p[i] != '"') { out[i] = p[i]; i++; } out[i] = '\0'; return 0;
}
static int fix_application_drm_type(const char* path) {
    FILE* f = fopen(path, "rb+"); if (!f) return -1;
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > 1024 * 1024 * 5) { fclose(f); return -1; } 
    char* buf = (char*)malloc(len + 1); fread(buf, 1, len, f); buf[len] = '\0'; 
    const char* key = "\"applicationDrmType\""; char* p = strstr(buf, key);
    if (!p) { free(buf); fclose(f); return 0; }
    char* colon = strchr(p + strlen(key), ':'); char* q1 = colon ? strchr(colon, '"') : NULL; char* q2 = q1 ? strchr(q1 + 1, '"') : NULL;
    if (!q1 || !q2) { free(buf); fclose(f); return -1; }
    if ((q2 - q1 - 1) == strlen("standard") && !strncmp(q1 + 1, "standard", strlen("standard"))) { free(buf); fclose(f); return 0; }
    size_t new_len = (q1 - buf) + 1 + strlen("standard") + 1 + strlen(q2 + 1);
    char* out = (char*)malloc(new_len + 1);
    memcpy(out, buf, q1 - buf + 1); memcpy(out + (q1 - buf + 1), "standard", strlen("standard")); strcpy(out + (q1 - buf + 1 + strlen("standard")), q2);
    fseek(f, 0, SEEK_SET); fwrite(out, 1, strlen(out), f); fclose(f); 
    free(buf); free(out); return 1;
}

bool get_game_info(const char* base_path, char* out_id, char* out_name) {
    char path[MAX_PATH]; snprintf(path, sizeof(path), "%s/sce_sys/param.json", base_path);
    fix_application_drm_type(path); 
    FILE* f = fopen(path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
        if (len > 0) {
            char* buf = (char*)malloc(len + 1);
            if (buf) {
                fread(buf, 1, len, f); buf[len] = '\0';
                int res = extract_json_string(buf, "titleId", out_id, MAX_TITLE_ID);
                if (res != 0) res = extract_json_string(buf, "title_id", out_id, MAX_TITLE_ID);
                if (res == 0) {
                    const char* en_ptr = strstr(buf, "\"en-US\""); const char* search_start = en_ptr ? en_ptr : buf;
                    if (extract_json_string(search_start, "titleName", out_name, MAX_TITLE_NAME) != 0) extract_json_string(buf, "titleName", out_name, MAX_TITLE_NAME);
                    if (strlen(out_name) == 0) strncpy(out_name, out_id, MAX_TITLE_NAME);
                    free(buf); fclose(f); return true;
                }
                free(buf);
            }
        }
        fclose(f);
    }
    return false;
}

// --- COUNTING PHASE ---
int count_new_candidates() {
    int count = 0;
    for (int i = 0; SCAN_PATHS[i] != NULL; i++) {
        DIR* d = opendir(SCAN_PATHS[i]); if (!d) continue; 
        struct dirent* entry;
        while ((entry = readdir(d)) != NULL) { 
            if (entry->d_name[0] == '.') continue; 
            char full_path[MAX_PATH]; snprintf(full_path, sizeof(full_path), "%s/%s", SCAN_PATHS[i], entry->d_name); 

            char title_id[MAX_TITLE_ID]; char title_name[MAX_TITLE_NAME];
            if (!get_game_info(full_path, title_id, title_name)) continue; 
            
            bool already_seen = false;
            for(int k=0; k<MAX_PENDING; k++) {
                if (cache[k].valid && strcmp(cache[k].path, full_path) == 0) { already_seen = true; break; }
            }
            if (already_seen) continue;

            count++;
        }
        closedir(d);
    }
    return count;
}

bool mount_and_install(const char* src_path, const char* title_id, const char* title_name) {
    char system_ex_app[MAX_PATH]; char user_app_dir[MAX_PATH]; char user_sce_sys[MAX_PATH]; char src_sce_sys[MAX_PATH];
    
    // MOUNT
    snprintf(system_ex_app, sizeof(system_ex_app), "/system_ex/app/%s", title_id); 
    mkdir(system_ex_app, 0777); remount_system_ex(); unmount(system_ex_app, 0); 
    if (mount_nullfs(src_path, system_ex_app) < 0) { log_debug("  [MOUNT] FAIL: %s", strerror(errno)); return false; }

    // COPY FILES
    snprintf(user_app_dir, sizeof(user_app_dir), "/user/app/%s", title_id); 
    snprintf(user_sce_sys, sizeof(user_sce_sys), "%s/sce_sys", user_app_dir);
    mkdir(user_app_dir, 0777); 
    mkdir(user_sce_sys, 0777);

    snprintf(src_sce_sys, sizeof(src_sce_sys), "%s/sce_sys", src_path); 
    copy_dir(src_sce_sys, user_sce_sys); 
    
    char icon_src[MAX_PATH], icon_dst[MAX_PATH]; 
    snprintf(icon_src, sizeof(icon_src), "%s/sce_sys/icon0.png", src_path);
    snprintf(icon_dst, sizeof(icon_dst), "/user/app/%s/icon0.png", title_id); 
    copy_file(icon_src, icon_dst);

    // WRITE TRACKER
    char lnk_path[MAX_PATH]; snprintf(lnk_path, sizeof(lnk_path), "/user/app/%s/mount.lnk", title_id);
    FILE* flnk = fopen(lnk_path, "w"); if (flnk) { fprintf(flnk, "%s", src_path); fclose(flnk); }
    
    // REGISTER
    int res = sceAppInstUtilAppInstallTitleDir(title_id, "/user/app/", 0);
    sceKernelUsleep(200000); 

    if (res == 0) { 
        log_debug("  [REG] Installed NEW!"); 
        trigger_rich_toast(title_id, title_name, "Installed"); 
    }
    else if (res == 0x80990002) { 
        log_debug("  [REG] Restored."); 
        trigger_rich_toast(title_id, title_name, "Restored"); 
    }
    else { log_debug("  [REG] FAIL: 0x%x", res); return false; }
    return true;
}

void scan_all_paths() {
    
    // Simple Cache Cleaner
    for(int k=0; k<MAX_PENDING; k++) {
        if (cache[k].valid) {
            if (access(cache[k].path, F_OK) != 0) {
                log_debug("  [CACHE] Path gone: %s", cache[k].path);
                cache[k].valid = false;
            }
        }
    }

    for (int i = 0; SCAN_PATHS[i] != NULL; i++) {
        DIR* d = opendir(SCAN_PATHS[i]); if (!d) continue; 
        struct dirent* entry;
        while ((entry = readdir(d)) != NULL) { 
            if (entry->d_name[0] == '.') continue; 
            char full_path[MAX_PATH]; snprintf(full_path, sizeof(full_path), "%s/%s", SCAN_PATHS[i], entry->d_name); 
            
            bool already_seen = false;
            for(int k=0; k<MAX_PENDING; k++) {
                if (cache[k].valid && strcmp(cache[k].path, full_path) == 0) { already_seen = true; break; }
            }
            if (already_seen) continue; 

            char title_id[MAX_TITLE_ID]; char title_name[MAX_TITLE_NAME];
            if (get_game_info(full_path, title_id, title_name)) {
                for(int k=0; k<MAX_PENDING; k++) {
                    if (!cache[k].valid) {
                        strncpy(cache[k].path, full_path, MAX_PATH);
                        strncpy(cache[k].title_id, title_id, MAX_TITLE_ID);
                        strncpy(cache[k].title_name, title_name, MAX_TITLE_NAME);
                        cache[k].valid = true;
                        break;
                    }
                }
            } else { continue; }

            // Skip if already mounted
            if (is_installed(title_id) && is_data_mounted(title_id)) {
                log_debug("  [SKIP] %s (Ready)", title_name);
                continue; 
            }

            notify_system("Found New Game: Installing %s...", title_name);
            log_debug("  [ACTION] Installing: %s", title_name);
            
            if (!wait_for_stability(full_path, title_name)) continue;
            mount_and_install(full_path, title_id, title_name);
        }
        closedir(d);
    }
}

int main() {
    sceAppInstUtilInitialize();
    remove(LOCK_FILE); 
    remove(LOG_FILE); mkdir(LOG_DIR, 0777);
    log_debug("SHADOWMOUNT v1.2 Beta (VOIDWHISPER EDITION) START");
    
    notify_system("ShadowMount v1.2 Beta by VoidWhisper Loaded");

    int new_games = count_new_candidates();
    if (new_games > 0) {
        notify_system("Found %d new games. Starting...", new_games);
    } else {
        notify_system("No new dumps found. Monitoring...");
    }

    int lock = open(LOCK_FILE, O_CREAT | O_EXCL | O_RDWR, 0666);
    if (lock < 0 && errno == EEXIST) { log_debug("FATAL: Daemon running."); return 0; }

    bool first_run = true;
    while (true) {
        if (access(KILL_FILE, F_OK) == 0) { remove(KILL_FILE); remove(LOCK_FILE); return 0; }
        
        scan_all_paths();
        
        if (first_run && new_games > 0) {
            notify_system("All dumps configured. Enjoy! - VoidWhisper");
            first_run = false;
        }

        sceKernelUsleep(SCAN_INTERVAL_US);
    }
    return 0;
}