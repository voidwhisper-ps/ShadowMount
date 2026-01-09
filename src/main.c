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

// Scan Paths
const char* SCAN_PATHS[] = {
    // Internal
    "/data/homebrew", "/data/etaHEN/games",
    
    // USB Subfolders
    "/mnt/usb0/homebrew", "/mnt/usb1/homebrew", "/mnt/usb2/homebrew", "/mnt/usb3/homebrew",
    "/mnt/usb4/homebrew", "/mnt/usb5/homebrew", "/mnt/usb6/homebrew", "/mnt/usb7/homebrew",
    
    "/mnt/usb0/etaHEN/games", "/mnt/usb1/etaHEN/games", "/mnt/usb2/etaHEN/games", "/mnt/usb3/etaHEN/games",
    "/mnt/usb4/etaHEN/games", "/mnt/usb5/etaHEN/games", "/mnt/usb6/etaHEN/games", "/mnt/usb7/etaHEN/games",

    // USB Root Paths
    "/mnt/usb0", "/mnt/usb1", "/mnt/usb2", "/mnt/usb3",
    "/mnt/usb4", "/mnt/usb5", "/mnt/usb6", "/mnt/usb7",
    "/mnt/ext0", "/mnt/ext1",
    
    NULL
};

struct GameCache { 
    char path[MAX_PATH]; 
    char title_id[MAX_TITLE_ID]; 
    char title_name[MAX_TITLE_NAME]; 
    bool valid; 
};
struct GameCache cache[MAX_PENDING];

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

// --- NOTIFICATIONS ---
void notify_system(const char* fmt, ...) {
    notify_request_t req; memset(&req, 0, sizeof(req));
    va_list args; va_start(args, fmt); vsnprintf(req.message, sizeof(req.message), fmt, args); va_end(args);
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
    log_debug("NOTIFY: %s", req.message);
}

void trigger_rich_toast(const char* title_id, const char* game_name, const char* msg) {
    FILE* f = fopen(TOAST_FILE, "w");
    if (f) {
        fprintf(f, "%s|%s|%s", title_id, game_name, msg);
        fflush(f); fclose(f);
    }
}

// --- FILESYSTEM ---
bool is_installed(const char* title_id) { char path[MAX_PATH]; snprintf(path, sizeof(path), "/user/app/%s", title_id); struct stat st; return (stat(path, &st) == 0); }
bool is_data_mounted(const char* title_id) { char path[MAX_PATH]; snprintf(path, sizeof(path), "/system_ex/app/%s/sce_sys/param.json", title_id); return (access(path, F_OK) == 0); }

// --- FAST STABILITY CHECK ---
bool wait_for_stability_fast(const char* path, const char* name) {
    struct stat st;
    time_t now = time(NULL);

    // 1. Check Root Folder Timestamp
    if (stat(path, &st) != 0) return false; 
    double diff = difftime(now, st.st_mtime);

    // If modified > 10 seconds ago, it's stable.
    if (diff > 10.0) {
        // Double check sce_sys just to be sure
        char sys_path[MAX_PATH];
        snprintf(sys_path, sizeof(sys_path), "%s/sce_sys", path);
        if (stat(sys_path, &st) == 0) {
            if (difftime(now, st.st_mtime) > 10.0) {
                 return true;
            }
        } else {
             return true; // No sce_sys? Trust root.
        }
    }
    
    
    log_debug("  [WAIT] %s modified %.0fs ago. Waiting...", name, diff);
    sceKernelUsleep(2000000); // Wait 2s
    return false; // Force re-scan next cycle
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

// --- COUNTING ---
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
            if (is_installed(title_id) && is_data_mounted(title_id)) continue; 

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

bool mount_and_install(const char* src_path, const char* title_id, const char* title_name, bool is_remount) {
    char system_ex_app[MAX_PATH]; char user_app_dir[MAX_PATH]; char user_sce_sys[MAX_PATH]; char src_sce_sys[MAX_PATH];
    
    // MOUNT
    snprintf(system_ex_app, sizeof(system_ex_app), "/system_ex/app/%s", title_id); 
    mkdir(system_ex_app, 0777); remount_system_ex(); unmount(system_ex_app, 0); 
    if (mount_nullfs(src_path, system_ex_app) < 0) { log_debug("  [MOUNT] FAIL: %s", strerror(errno)); return false; }

    // COPY FILES
    if (!is_remount) {
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
    } else {
        log_debug("  [SPEED] Skipping file copy (Assets already exist)");
    }

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
        // Silent on restore/remount to avoid spam
    }
    else { log_debug("  [REG] FAIL: 0x%x", res); return false; }
    return true;
}

void scan_all_paths() {
    
    // Cache Cleaner
    for(int k=0; k<MAX_PENDING; k++) {
        if (cache[k].valid) {
            if (access(cache[k].path, F_OK) != 0) {
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

            // 1. Skip if perfect
            bool installed = is_installed(title_id);
            if (installed && is_data_mounted(title_id)) {
                continue; 
            }

            // 2. Decide Action
            bool is_remount = false;
            if (installed) {
                log_debug("  [ACTION] Remounting: %s", title_name);
                // NOTIFICATION REMOVED FOR REMOUNT
                is_remount = true;
            } else {
                log_debug("  [ACTION] Installing: %s", title_name);
                notify_system("Installing: %s...", title_name); 
                
                // FAST CHECK
                if (!wait_for_stability_fast(full_path, title_name)) continue;
                is_remount = false;
            }

            mount_and_install(full_path, title_id, title_name, is_remount);
        }
        closedir(d);
    }
}

int main() {
    // Initialize services
    sceUserServiceInitialize(0);
    sceAppInstUtilInitialize();
    kernel_set_ucred_authid(-1, 0x4801000000000013L);

    remove(LOCK_FILE); 
    remove(LOG_FILE); 
    mkdir(LOG_DIR, 0777);
    
    log_debug("SHADOWMOUNT v1.3 START");
    
    // --- STARTUP LOGIC ---
    int new_games = count_new_candidates();
    
    if (new_games == 0) {
        // SCENARIO A: Nothing to do.
        notify_system("ShadowMount v1.3: Library Ready.\n- VoidWhisper");
    } else {
        // SCENARIO B: Work needed.
        notify_system("ShadowMount v1.3: Found %d Games. Executing...", new_games);
        
        // Run the scan immediately to process them
        scan_all_paths();
        
        // Completion Message
        notify_system("Library Synchronized. - VoidWhisper");
    }

    // --- DAEMON LOOP ---
    int lock = open(LOCK_FILE, O_CREAT | O_EXCL | O_RDWR, 0666);
    if (lock < 0 && errno == EEXIST) { return 0; }

    while (true) {
        if (access(KILL_FILE, F_OK) == 0) { remove(KILL_FILE); remove(LOCK_FILE); return 0; }
        
        // Sleep FIRST since we either just finished scan above, or library was ready.
        sceKernelUsleep(SCAN_INTERVAL_US);
        
        scan_all_paths();
    }
    
    sceUserServiceTerminate();
    return 0;

}
