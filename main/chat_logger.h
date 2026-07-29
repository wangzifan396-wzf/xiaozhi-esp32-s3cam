#pragma once
#include <cstdio>
#include <ctime>
#include <sys/stat.h>
#include <esp_log.h>

#define TAG_CL "ChatLogger"

class ChatLogger {
    static bool first_message_;
public:
    static void OnBoot() { first_message_ = true; }

    static void Append(const char* role, const char* content) {
        if (!content || content[0] == '\0') return;
        mkdir("/sdcard/chats", 0755);

        time_t now = time(nullptr);
        struct tm timeinfo;
        localtime_r(&now, &timeinfo);
        char date[16], ts[32];
        strftime(date, sizeof(date), "%Y-%m-%d", &timeinfo);
        strftime(ts, sizeof(ts), "%H:%M:%S", &timeinfo);

        char filepath[64];
        snprintf(filepath, sizeof(filepath), "/sdcard/chats/%s.log", date);
        FILE* fp = fopen(filepath, "a");
        if (!fp) return;

        if (first_message_) {
            fprintf(fp, "\n--- 开机 %s ---\n\n", ts);
            first_message_ = false;
        }
        fprintf(fp, "[%s] %s: %s\n", ts, role, content);
        fclose(fp);
    }
};
bool ChatLogger::first_message_ = true;
