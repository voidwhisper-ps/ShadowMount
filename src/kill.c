#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h> // <--- THIS WAS MISSING

// SDK Imports
void sceUserServiceInitialize(void*);
void sceUserServiceTerminate(void);
typedef struct notify_request { char unused[45]; char message[3075]; } notify_request_t;
int sceKernelSendNotificationRequest(int, notify_request_t*, size_t, int);

void notify(const char* fmt, ...) {
    notify_request_t req; 
    memset(&req, 0, sizeof(req));
    va_list args; 
    va_start(args, fmt); 
    vsnprintf(req.message, sizeof(req.message), fmt, args); 
    va_end(args);
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

int main() {
    sceUserServiceInitialize(0);
    
    // Create the "Poison Pill" file
    // The main daemon checks for this file. If it exists, the daemon kills itself.
    int fd = open("/data/shadowmount.kill", O_CREAT | O_TRUNC | O_WRONLY, 0777);
    if (fd >= 0) {
        write(fd, "DIE", 3);
        close(fd);
        notify("ShadowMount: Kill Signal Sent!");
    } else {
        notify("Error: Could not create kill file!");
    }

    sceUserServiceTerminate();
    return 0;
}