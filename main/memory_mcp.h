#pragma once
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <dirent.h>
#include <sys/stat.h>
#include <esp_log.h>
#include "mcp_server.h"
#include "board.h"
#include "lvgl_display/lvgl_image.h"
#include "display/lvgl_display/lvgl_display.h"
#include <esp_netif.h>

#define TAG_MM "MemoryMCP"

// Photo browser state
static std::vector<std::string> g_photo_list;
static int g_photo_index = -1;

static void RefreshPhotoList() {
    g_photo_list.clear();
    // Scan /sdcard/photos/ subdirectories for .jpg files
    DIR* dir = opendir("/sdcard/photos");
    if (!dir) return;
    struct dirent* entry;
    while ((entry = readdir(dir))) {
        if (entry->d_name[0] == '.') continue;
        // Check if it's a directory (year-month folder like "2026-07")
        std::string subpath = std::string("/sdcard/photos/") + entry->d_name;
        DIR* subdir = opendir(subpath.c_str());
        if (subdir) {
            struct dirent* sub;
            while ((sub = readdir(subdir))) {
                if (strstr(sub->d_name, ".jpg") || strstr(sub->d_name, ".jpeg"))
                    g_photo_list.push_back(std::string(entry->d_name) + "/" + sub->d_name);
            }
            closedir(subdir);
        }
    }
    closedir(dir);
    std::sort(g_photo_list.begin(), g_photo_list.end());
    ESP_LOGI(TAG_MM, "Photo list: %d photos", (int)g_photo_list.size());
}

#include <img_converters.h>

// Parse JPEG dimensions from header (SOF0/SOF2 marker)
static bool JpegGetSize(const uint8_t* data, size_t size, int* w, int* h) {
    if (size < 4 || data[0] != 0xFF || data[1] != 0xD8) return false;
    size_t i = 2;
    while (i < size - 9) {
        if (data[i] != 0xFF) { i++; continue; }
        uint8_t m = data[i+1];
        if (m == 0xC0 || m == 0xC2) { // SOF0 or SOF2
            *h = (data[i+5] << 8) | data[i+6];
            *w = (data[i+7] << 8) | data[i+8];
            return *w > 0 && *h > 0;
        }
        i += 2 + ((data[i+2] << 8) | data[i+3]);
    }
    return false;
}

static bool ShowCurrentPhoto() {
    if (g_photo_index < 0 || g_photo_index >= (int)g_photo_list.size()) return false;
    std::string path = "/sdcard/photos/" + g_photo_list[g_photo_index];
    FILE* fp = fopen(path.c_str(), "rb");
    if (!fp) return false;
    fseek(fp, 0, SEEK_END);
    size_t jpg_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t* jpg_data = (uint8_t*)heap_caps_malloc(jpg_size, MALLOC_CAP_SPIRAM);
    if (!jpg_data) { fclose(fp); return false; }
    fread(jpg_data, 1, jpg_size, fp);
    fclose(fp);

    // Get JPEG dimensions
    int w, h;
    if (!JpegGetSize(jpg_data, jpg_size, &w, &h)) {
        ESP_LOGW(TAG_MM, "Failed to parse JPEG header");
        heap_caps_free(jpg_data);
        return false;
    }

    // Decode JPEG to RGB888 using esp32-camera's fmt2rgb888
    size_t rgb_size = w * h * 3;
    uint8_t* rgb = (uint8_t*)heap_caps_malloc(rgb_size, MALLOC_CAP_SPIRAM);
    if (!rgb) { heap_caps_free(jpg_data); return false; }
    if (!fmt2rgb888(jpg_data, jpg_size, PIXFORMAT_JPEG, rgb)) {
        ESP_LOGW(TAG_MM, "JPEG decode failed");
        heap_caps_free(rgb);
        heap_caps_free(jpg_data);
        return false;
    }
    heap_caps_free(jpg_data);

    // Scale to fit 240x240 screen (max dimension)
    int dw = w, dh = h;
    if (dw > 240 || dh > 240) {
        float scale = 240.0f / (dw > dh ? dw : dh);
        dw = (int)(w * scale);
        dh = (int)(h * scale);
    }
    // Downscale RGB888 → scaled RGB565
    size_t rgb565_size = dw * dh * 2;
    uint16_t* rgb565 = (uint16_t*)heap_caps_malloc(rgb565_size, MALLOC_CAP_SPIRAM);
    if (!rgb565) { heap_caps_free(rgb); return false; }
    for (int y = 0; y < dh; y++) {
        for (int x = 0; x < dw; x++) {
            int sx = x * w / dw, sy = y * h / dh;
            uint8_t r = rgb[(sy * w + sx) * 3];
            uint8_t g = rgb[(sy * w + sx) * 3 + 1];
            uint8_t b = rgb[(sy * w + sx) * 3 + 2];
            rgb565[y * dw + x] = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
        }
    }
    heap_caps_free(rgb);

    auto display = dynamic_cast<LvglDisplay*>(Board::GetInstance().GetDisplay());
    if (display) {
        display->SetPreviewImage(std::make_unique<LvglAllocatedImage>(rgb565, rgb565_size, dw, dh, dw * 2, LV_COLOR_FORMAT_RGB565));
        ESP_LOGI(TAG_MM, "Showing photo %d/%d: %s (%dx%d → %dx%d)", g_photo_index+1, (int)g_photo_list.size(), g_photo_list[g_photo_index].c_str(), w, h, dw, dh);
        return true;
    }
    heap_caps_free(rgb565);
    return false;
}

// Register MCP tools
inline void MemoryMCP_Register() {
    // Tool: search chat history (scans all .log files in /sdcard/chats/)
    McpServer::GetInstance().AddTool(
        "self.memory.search",
        "Search all saved chat history on SD card for a keyword. Searches across all dates.",
        PropertyList({Property("keyword", kPropertyTypeString)}),
        [](const PropertyList& props) -> ReturnValue {
            std::string keyword;
            try { keyword = props["keyword"].value<std::string>(); }
            catch (...) { return std::string("{\"error\":\"Missing keyword\"}"); }
            std::string result = "{\"matches\":[";
            char line[512]; int count = 0; bool first = true;
            // Scan all .log files in /sdcard/chats/
            DIR* dir = opendir("/sdcard/chats");
            if (dir) {
                struct dirent* entry;
                while ((entry = readdir(dir)) && count < 15) {
                    if (!strstr(entry->d_name, ".log")) continue;
                    std::string path = "/sdcard/chats/" + std::string(entry->d_name);
                    FILE* fp = fopen(path.c_str(), "r");
                    if (!fp) continue;
                    while (fgets(line, sizeof(line), fp) && count < 15) {
                        std::string l(line), k(keyword);
                        for (auto& c : l) c = tolower(c);
                        for (auto& c : k) c = tolower(c);
                        if (l.find(k) != std::string::npos) {
                            if (!first) result += ",";
                            size_t len = strlen(line);
                            if (len && line[len-1]=='\n') line[len-1]=0;
                            std::string esc;
                            for (char* p=line; *p; p++) { if (*p=='"') esc+='\\'; esc+=*p; }
                            result += "\"" + esc + "\"";
                            first = false; count++;
                        }
                    }
                    fclose(fp);
                }
                closedir(dir);
            }
            result += "],\"count\":" + std::to_string(count) + "}";
            return result;
        });

    // Tool: recent messages (reads all date-based .log files, returns last N lines)
    McpServer::GetInstance().AddTool(
        "self.memory.recent",
        "Get the most recent N chat messages from SD card.",
        PropertyList({Property("count", kPropertyTypeInteger, 5)}),
        [](const PropertyList& props) -> ReturnValue {
            int n = 5;
            try { n = props["count"].value<int>(); } catch (...) {}
            // Collect all .log files sorted by name (which is date)
            std::vector<std::string> files;
            DIR* dir = opendir("/sdcard/chats");
            if (dir) {
                struct dirent* entry;
                while ((entry = readdir(dir)))
                    if (strstr(entry->d_name, ".log"))
                        files.push_back(entry->d_name);
                closedir(dir);
            }
            std::sort(files.begin(), files.end());
            // Read lines from all files, keep last N
            std::vector<std::string> lines;
            char line[512];
            for (auto& fname : files) {
                std::string path = "/sdcard/chats/" + fname;
                FILE* fp = fopen(path.c_str(), "r");
                if (!fp) continue;
                while (fgets(line, sizeof(line), fp)) {
                    size_t len = strlen(line);
                    if (len && line[len-1]=='\n') line[len-1]=0;
                    lines.push_back(line);
                    if ((int)lines.size() > n) lines.erase(lines.begin());
                }
                fclose(fp);
            }
            std::string result = "{\"recent\":[";
            for (size_t i=0; i<lines.size(); i++) {
                if (i) result += ",";
                std::string esc;
                for (char c : lines[i]) { if (c=='"') esc+='\\'; esc+=c; }
                result += "\"" + esc + "\"";
            }
            result += "]}";
            return result;
        });

    // Tool: list photos
    McpServer::GetInstance().AddTool(
        "self.memory.photos",
        "List photos saved on SD card. Use show=N to display a specific photo on screen, or just list them.",
        PropertyList({Property("show", kPropertyTypeInteger, -1)}),
        [](const PropertyList& props) -> ReturnValue {
            int show = -1;
            try { show = props["show"].value<int>(); } catch (...) {}
            RefreshPhotoList();
            if (show >= 0 && show < (int)g_photo_list.size()) {
                g_photo_index = show;
                ShowCurrentPhoto();
            }
            std::string result = "{\"photos\":[";
            for (size_t i=0; i<g_photo_list.size(); i++) {
                if (i) result += ",";
                result += "\"" + g_photo_list[i] + "\"";
            }
            result += "],\"count\":" + std::to_string(g_photo_list.size());
            if (g_photo_index >= 0)
                result += ",\"current\":" + std::to_string(g_photo_index + 1) + ",\"current_name\":\"" + g_photo_list[g_photo_index] + "\"";
            result += "}";
            return result;
        });

    // Tool: show photo by filename
    McpServer::GetInstance().AddTool(
        "self.memory.show_photo",
        "Show a specific photo on the screen by filename or by its position number (1=oldest).",
        PropertyList({Property("name_or_index", kPropertyTypeString)}),
        [](const PropertyList& props) -> ReturnValue {
            std::string key;
            try { key = props["name_or_index"].value<std::string>(); }
            catch (...) { return std::string("{\"error\":\"Missing name_or_index\"}"); }
            RefreshPhotoList();
            // Try to match by exact filename
            for (int i=0; i<(int)g_photo_list.size(); i++) {
                if (g_photo_list[i] == key) {
                    g_photo_index = i;
                    ShowCurrentPhoto();
                    return std::string("{\"shown\":\"" + g_photo_list[i] + "\",\"index\":" + std::to_string(i+1) + "}");
                }
            }
            // Try to match by 1-based index
            int idx = atoi(key.c_str());
            if (idx >= 1 && idx <= (int)g_photo_list.size()) {
                g_photo_index = idx - 1;
                ShowCurrentPhoto();
                return std::string("{\"shown\":\"" + g_photo_list[g_photo_index] + "\",\"index\":" + std::to_string(idx) + "}");
            }
            return std::string("{\"error\":\"Photo not found: " + key + "\"}");
        });

    // Tool: next photo
    McpServer::GetInstance().AddTool(
        "self.memory.next_photo",
        "Show the next photo on the screen. Auto-refreshes the photo list.",
        PropertyList(),
        [](const PropertyList& props) -> ReturnValue {
            RefreshPhotoList();
            if (g_photo_list.empty()) return std::string("{\"error\":\"No photos found\"}");
            if (g_photo_index < 0) g_photo_index = 0;
            else g_photo_index = (g_photo_index + 1) % g_photo_list.size();
            ShowCurrentPhoto();
            return std::string("{\"shown\":\"" + g_photo_list[g_photo_index] + "\",\"index\":" + std::to_string(g_photo_index+1) + ",\"total\":" + std::to_string(g_photo_list.size()) + "}");
        });

    // Tool: previous photo
    McpServer::GetInstance().AddTool(
        "self.memory.prev_photo",
        "Show the previous photo on the screen.",
        PropertyList(),
        [](const PropertyList& props) -> ReturnValue {
            RefreshPhotoList();
            if (g_photo_list.empty()) return std::string("{\"error\":\"No photos found\"}");
            if (g_photo_index < 0) g_photo_index = g_photo_list.size() - 1;
            else g_photo_index = (g_photo_index - 1 + g_photo_list.size()) % g_photo_list.size();
            ShowCurrentPhoto();
            return std::string("{\"shown\":\"" + g_photo_list[g_photo_index] + "\",\"index\":" + std::to_string(g_photo_index+1) + ",\"total\":" + std::to_string(g_photo_list.size()) + "}");
        });

    // === Voice Memos ===
    mkdir("/sdcard/memos", 0755);

    // Save a memo
    McpServer::GetInstance().AddTool(
        "self.memo.save",
        "Save a voice memo / note to SD card. Use when the user asks to remember something.",
        PropertyList({
            Property("title", kPropertyTypeString),
            Property("content", kPropertyTypeString),
        }),
        [](const PropertyList& props) -> ReturnValue {
            std::string title, content;
            try { title = props["title"].value<std::string>();
                  content = props["content"].value<std::string>(); }
            catch (...) { return std::string("{\"error\":\"Missing title or content\"}"); }
            // Sanitize filename
            for (auto& c : title) if (c=='/'||c=='\\'||c==':'||c=='*'||c=='?'||c=='"'||c=='<'||c=='>'||c=='|') c='_';
            std::string path = "/sdcard/memos/" + title + ".txt";
            FILE* fp = fopen(path.c_str(), "w");
            if (!fp) return std::string("{\"error\":\"Failed to save memo\"}");
            fprintf(fp, "%s\n", content.c_str());
            fclose(fp);
            ESP_LOGI(TAG_MM, "Memo saved: %s", title.c_str());
            return std::string("{\"saved\":\"" + title + "\"}");
        });

    // Recall a memo
    McpServer::GetInstance().AddTool(
        "self.memo.recall",
        "Recall a saved memo/note by its title.",
        PropertyList({Property("title", kPropertyTypeString)}),
        [](const PropertyList& props) -> ReturnValue {
            std::string title;
            try { title = props["title"].value<std::string>(); }
            catch (...) { return std::string("{\"error\":\"Missing title\"}"); }
            for (auto& c : title) if (c=='/'||c=='\\'||c==':'||c=='*'||c=='?'||c=='"'||c=='<'||c=='>'||c=='|') c='_';
            std::string path = "/sdcard/memos/" + title + ".txt";
            FILE* fp = fopen(path.c_str(), "r");
            if (!fp) return std::string("{\"error\":\"Memo not found: " + title + "\"}");
            char buf[1024];
            size_t n = fread(buf, 1, sizeof(buf)-1, fp);
            fclose(fp);
            buf[n] = 0;
            return std::string("{\"title\":\"" + title + "\",\"content\":\"" + std::string(buf) + "\"}");
        });

    // List all memos
    McpServer::GetInstance().AddTool(
        "self.memo.list",
        "List all saved memos/notes.",
        PropertyList(),
        [](const PropertyList& props) -> ReturnValue {
            DIR* dir = opendir("/sdcard/memos");
            if (!dir) return std::string("{\"memos\":[],\"count\":0}");
            std::string result = "{\"memos\":[";
            struct dirent* entry;
            int count = 0;
            bool first = true;
            while ((entry = readdir(dir))) {
                if (!strstr(entry->d_name, ".txt")) continue;
                if (!first) result += ",";
                std::string name = entry->d_name;
                name = name.substr(0, name.size() - 4); // strip .txt
                result += "\"" + name + "\"";
                first = false; count++;
            }
            closedir(dir);
            result += "],\"count\":" + std::to_string(count) + "}";
            return result;
        });

    // Tool: get device IP for WiFi file access
    McpServer::GetInstance().AddTool(
        "self.memory.device_ip",
        "Get the device's IP address so the user can access files via browser.",
        PropertyList(),
        [](const PropertyList& props) -> ReturnValue {
            esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            if (!netif) return std::string("{\"error\":\"WiFi not connected\"}");
            esp_netif_ip_info_t ip_info;
            if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK)
                return std::string("{\"error\":\"Cannot get IP\"}");
            char ip[16];
            snprintf(ip, sizeof(ip), IPSTR, IP2STR(&ip_info.ip));
            return std::string("{\"ip\":\"" + std::string(ip) + "\",\"url\":\"http://" + std::string(ip) + ":8080\"}");
        });

    ESP_LOGI(TAG_MM, "Memory MCP registered: search, recent, photos, browse, memos, device_ip");
}
