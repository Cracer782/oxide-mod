 #include <jni.h>
#include <string>
#include <thread>
#include <chrono>
#include <atomic>
#include <csignal>
#include <pthread.h>
#include <android/log.h>
#include <sys/mman.h>
#include <unistd.h>
#include <link.h>
#include <dlfcn.h>
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_android.h"

#define LOG_TAG "OxideCheat"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

static std::atomic<bool> g_running{true};
static JavaVM* g_vm = nullptr;

static bool IsDebuggerAttached() {
    FILE* f = fopen("/proc/self/status", "r");
    if (!f) return false;
    char buf[256];
    bool found = false;
    while (fgets(buf, sizeof(buf), f)) {
        if (strncmp(buf, "TracerPid:", 10) == 0) {
            found = atoi(buf + 10) != 0;
            break;
        }
    }
    fclose(f);
    return found;
}

static void HideFromMaps(const char* name) {
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, name)) {
            char* p = strchr(line, '/');
            if (p) memset(p, ' ', strlen(p) - 1);
        }
    }
    fclose(f);
}

static void* MainThread(void*) {
    std::this_thread::sleep_for(std::chrono::seconds(3));
    if (IsDebuggerAttached()) return nullptr;
    LOGI("[+] Thread started");
    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return nullptr;
}

jint JNI_OnLoad(JavaVM* vm, void* reserved) {
    g_vm = vm;
    signal(SIGPIPE, SIG_IGN);
    HideFromMaps("libmodmenu.so");
    pthread_t t;
    if (pthread_create(&t, nullptr, MainThread, nullptr) == 0)
        pthread_detach(t);
    return JNI_VERSION_1_6;
}

void JNI_OnUnload(JavaVM* vm, void* reserved) {
    g_running.store(false);
    g_vm = nullptr;
}
