#pragma once
#include <cstdio>
#include <cstring>
#include <string>
#include <esp_log.h>
#include <esp_timer.h>
#include "mcp_server.h"
#include "board.h"
#include "camera.h"

#define TAG_ADV "AdvancedMCP"

// ========== Camera Timer ==========
static esp_timer_handle_t g_photo_timer = nullptr;
static esp_timer_handle_t g_photo_delay_timer = nullptr;
static int g_photo_count = 0, g_photo_total = 0;
static uint64_t g_photo_interval_us = 0;

static void TakeOnePhoto() {
    auto cam = Board::GetInstance().GetCamera();
    if (cam) {
        cam->Capture();
        try { cam->Explain(""); }
        catch (...) {}
    }
}

static void PhotoIntervalCallback(void*) {
    TakeOnePhoto();
    g_photo_count++;
    if (g_photo_count >= g_photo_total) {
        esp_timer_stop(g_photo_timer);
        esp_timer_delete(g_photo_timer);
        g_photo_timer = nullptr;
        ESP_LOGI(TAG_ADV, "Photo burst done: %d photos", g_photo_count);
    }
}

static void PhotoDelayCallback(void*) {
    esp_timer_delete(g_photo_delay_timer);
    g_photo_delay_timer = nullptr;
    TakeOnePhoto();
    g_photo_count++;
    if (g_photo_total <= 1) return;
    esp_timer_create_args_t args = {.callback = PhotoIntervalCallback, .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK, .name = "photo_interval"};
    esp_timer_create(&args, &g_photo_timer);
    esp_timer_start_periodic(g_photo_timer, g_photo_interval_us);
}

inline void AdvancedMCP_Register() {
    McpServer::GetInstance().AddTool(
        "self.camera.take_photos",
        "Take photos with flexible timing. count=1 delay_sec=0 interval_sec=0 = single photo now. "
        "count=1 delay_sec=3 = one photo after 3 seconds. "
        "count=3 delay_sec=0 interval_sec=5 = 3 photos 5 seconds apart. "
        "count=3 delay_sec=30 interval_sec=2 = wait 30s then 3 photos 2s apart. "
        "count=0 = unlimited until stopped.",
        PropertyList({
            Property("count", kPropertyTypeInteger, 1),
            Property("delay_sec", kPropertyTypeInteger, 0),
            Property("interval_sec", kPropertyTypeInteger, 0),
        }),
        [](const PropertyList& props) -> ReturnValue {
            int count = 1, delay_sec = 0, interval = 0;
            try { count = props["count"].value<int>();
                  delay_sec = props["delay_sec"].value<int>();
                  interval = props["interval_sec"].value<int>(); }
            catch (...) {}
            if (count < 0) count = 0;
            if (delay_sec < 0) delay_sec = 0;
            if (delay_sec > 300) delay_sec = 300;
            if (interval < 0) interval = 0;
            if (interval > 3600) interval = 3600;
            if (g_photo_timer) { esp_timer_stop(g_photo_timer); esp_timer_delete(g_photo_timer); g_photo_timer = nullptr; }
            if (g_photo_delay_timer) { esp_timer_stop(g_photo_delay_timer); esp_timer_delete(g_photo_delay_timer); g_photo_delay_timer = nullptr; }
            g_photo_count = 0;
            g_photo_total = (count == 0) ? 999999 : count;
            g_photo_interval_us = (uint64_t)interval * 1000000ULL;
            if (delay_sec > 0) {
                esp_timer_create_args_t args = {.callback = PhotoDelayCallback, .arg = nullptr,
                    .dispatch_method = ESP_TIMER_TASK, .name = "photo_delay"};
                esp_timer_create(&args, &g_photo_delay_timer);
                esp_timer_start_once(g_photo_delay_timer, (uint64_t)delay_sec * 1000000ULL);
                ESP_LOGI(TAG_ADV, "Photo: delay=%ds, count=%d, interval=%ds", delay_sec, count, interval);
            } else if (interval > 0) {
                TakeOnePhoto(); g_photo_count++;
                esp_timer_create_args_t args = {.callback = PhotoIntervalCallback, .arg = nullptr,
                    .dispatch_method = ESP_TIMER_TASK, .name = "photo_interval"};
                esp_timer_create(&args, &g_photo_timer);
                esp_timer_start_periodic(g_photo_timer, g_photo_interval_us);
            } else {
                TakeOnePhoto();
            }
            return std::string("{\"started\":true,\"count\":" + std::to_string(count) +
                ",\"delay\":" + std::to_string(delay_sec) + ",\"interval\":" + std::to_string(interval) + "}");
        });

    McpServer::GetInstance().AddTool(
        "self.camera.stop_photos",
        "Stop any ongoing photo capture sequence.",
        PropertyList(),
        [](const PropertyList& props) -> ReturnValue {
            if (g_photo_timer) { esp_timer_stop(g_photo_timer); esp_timer_delete(g_photo_timer); g_photo_timer = nullptr; }
            if (g_photo_delay_timer) { esp_timer_stop(g_photo_delay_timer); esp_timer_delete(g_photo_delay_timer); g_photo_delay_timer = nullptr; }
            return std::string("{\"stopped\":true,\"taken\":" + std::to_string(g_photo_count) + "}");
        });

    ESP_LOGI(TAG_ADV, "Advanced MCP: take_photos, stop_photos");
}
