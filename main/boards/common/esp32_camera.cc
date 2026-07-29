#include "sdkconfig.h"

#include <esp_heap_caps.h>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <sys/stat.h>
#include <esp_log.h>
#include <img_converters.h>

#include "esp32_camera.h"
#include "board.h"
#include "display.h"
#include "lvgl_display.h"
#include "mcp_server.h"
#include "system_info.h"
#include "jpg/image_to_jpeg.h"
#include "esp_timer.h"

#define TAG "Esp32Camera"

Esp32Camera::Esp32Camera(const camera_config_t &config) {
    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init failed with error 0x%x", err);
        return;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        if (s->id.PID == GC0308_PID) {
            s->set_hmirror(s, 0); // Control camera mirror: 1 for mirror, 0 for normal
        }
        ESP_LOGI(TAG, "Camera initialized: format=%d", config.pixel_format);
    }

    streaming_on_ = true;
}

Esp32Camera::~Esp32Camera() {
    if (streaming_on_) {
        if (current_fb_) {
            esp_camera_fb_return(current_fb_);
            current_fb_ = nullptr;
        }
        if (encode_buf_) {
            heap_caps_free(encode_buf_);
            encode_buf_ = nullptr;
            encode_buf_size_ = 0;
        }
        esp_camera_deinit();
        streaming_on_ = false;
    }
}

void Esp32Camera::SetExplainUrl(const std::string &url, const std::string &token) {
    explain_url_ = url;
    explain_token_ = token;
}

bool Esp32Camera::Capture() {
    if (encoder_thread_.joinable()) {
        encoder_thread_.join();
    }

    if (!streaming_on_) {
        return false;
    }

    // Get the latest frame, discard old frames for real-time performance
    for (int i = 0; i < 2; i++) {
        if (current_fb_) {
            esp_camera_fb_return(current_fb_);
        }
        current_fb_ = esp_camera_fb_get();
        if (!current_fb_) {
            ESP_LOGE(TAG, "Camera capture failed");
            return false;
        }
    }

    // Handle preview: decode JPEG to RGB565 for screen display
    if (current_fb_->format == PIXFORMAT_JPEG && current_fb_->len > 0) {
        // Use fmt2rgb888 to decode JPEG → RGB888, then convert to RGB565 for preview
        uint8_t* jpg_buf = current_fb_->buf;
        size_t jpg_len = current_fb_->len;
        // Get dimensions from JPEG header
        int pw = 640, ph = 480; // default VGA, fmt2rgb888 figures out actual size
        size_t rgb_size = pw * ph * 3;
        uint8_t* rgb = (uint8_t*)heap_caps_malloc(rgb_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (rgb && fmt2rgb888(jpg_buf, jpg_len, PIXFORMAT_JPEG, rgb)) {
            // Quick JPEG dimension detection for proper scaling
            // Use the JPEG data directly to find SOF marker
            if (jpg_len > 100 && jpg_buf[0] == 0xFF && jpg_buf[1] == 0xD8) {
                for (size_t i = 2; i < jpg_len - 9; i++) {
                    if (jpg_buf[i] == 0xFF) {
                        uint8_t m = jpg_buf[i+1];
                        if (m == 0xC0 || m == 0xC2) {
                            ph = (jpg_buf[i+5] << 8) | jpg_buf[i+6];
                            pw = (jpg_buf[i+7] << 8) | jpg_buf[i+8];
                            break;
                        }
                        if (m != 0xD8 && m != 0xD9) i += 1 + ((jpg_buf[i+2] << 8) | jpg_buf[i+3]);
                    }
                }
            }
            // Scale to fit 240x240 max for preview
            int dw = pw, dh = ph;
            if (dw > 240 || dh > 240) {
                float scale = 240.0f / (dw > dh ? dw : dh);
                dw = (int)(pw * scale); dh = (int)(ph * scale);
            }
            size_t out_size = dw * dh * 2;
            uint16_t* rgb565 = (uint16_t*)heap_caps_malloc(out_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (rgb565) {
                for (int y = 0; y < dh; y++) {
                    for (int x = 0; x < dw; x++) {
                        int sx = x * pw / dw, sy = y * ph / dh;
                        uint8_t r = rgb[(sy * pw + sx) * 3];
                        uint8_t g = rgb[(sy * pw + sx) * 3 + 1];
                        uint8_t b = rgb[(sy * pw + sx) * 3 + 2];
                        rgb565[y * dw + x] = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
                    }
                }
                auto display = dynamic_cast<LvglDisplay *>(Board::GetInstance().GetDisplay());
                if (display) {
                    display->SetPreviewImage(std::make_unique<LvglAllocatedImage>(rgb565, out_size, dw, dh, dw * 2, LV_COLOR_FORMAT_RGB565));
                } else {
                    heap_caps_free(rgb565);
                }
            }
        } else {
            ESP_LOGW(TAG, "JPEG preview decode failed");
        }
        if (rgb) heap_caps_free(rgb);
    }

    ESP_LOGI(TAG, "Captured frame: %dx%d, len=%zu, format=%d",
             current_fb_->width, current_fb_->height, current_fb_->len, current_fb_->format);

    return true;
}

bool Esp32Camera::SetHMirror(bool enabled) {
    sensor_t *s = esp_camera_sensor_get();
    if (!s) {
        return false;
    }
    s->set_hmirror(s, enabled ? 1 : 0);
    return true;
}

bool Esp32Camera::SetVFlip(bool enabled) {
    sensor_t *s = esp_camera_sensor_get();
    if (!s) {
        return false;
    }
    s->set_vflip(s, enabled ? 1 : 0);
    return true;
}

bool Esp32Camera::SetSwapBytes(bool enabled) {
    swap_bytes_enabled_ = enabled;
    return true;
}

std::string Esp32Camera::Explain(const std::string &question) {
    if (explain_url_.empty()) {
        throw std::runtime_error("Image explain URL or token is not set");
    }

    if (current_fb_ == nullptr) {
        throw std::runtime_error("No camera frame captured");
    }

    // JPEG format: camera already outputs JPEG, no encoding needed
    if (current_fb_->format == PIXFORMAT_JPEG) {
        // Save to SD card
        {
            time_t now = time(nullptr);
            struct tm timeinfo;
            localtime_r(&now, &timeinfo);
            char dir[48], filename[32], fullpath[128];
            strftime(dir, sizeof(dir), "/sdcard/photos/%Y-%m", &timeinfo);
            strftime(filename, sizeof(filename), "%Y%m%d_%H%M%S.jpg", &timeinfo);
            mkdir("/sdcard/photos", 0755);
            mkdir(dir, 0755);
            snprintf(fullpath, sizeof(fullpath), "%s/%s", dir, filename);
            FILE* fp = fopen(fullpath, "wb");
            if (fp) {
                fwrite(current_fb_->buf, 1, current_fb_->len, fp);
                fclose(fp);
                ESP_LOGI(TAG, "Photo saved to SD: %s (%zu bytes)", fullpath, current_fb_->len);
            }
        }

        // Upload via HTTP
        auto network = Board::GetInstance().GetNetwork();
        auto http = network->CreateHttp(3);
        std::string boundary = "----ESP32_CAMERA_BOUNDARY";
        http->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
        http->SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str());
        if (!explain_token_.empty())
            http->SetHeader("Authorization", "Bearer " + explain_token_);
        http->SetHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
        if (!http->Open("POST", explain_url_))
            throw std::runtime_error("Failed to connect");

        std::string q = "--" + boundary + "\r\nContent-Disposition: form-data; name=\"question\"\r\n\r\n" + question + "\r\n"
            + "--" + boundary + "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"camera.jpg\"\r\nContent-Type: image/jpeg\r\n\r\n";
        http->Write(q.c_str(), q.size());
        http->Write((const char*)current_fb_->buf, current_fb_->len);
        std::string footer = "\r\n--" + boundary + "--\r\n";
        http->Write(footer.c_str(), footer.size());
        http->Write("", 0);

        if (http->GetStatusCode() != 200) {
            ESP_LOGE(TAG, "Upload failed: %d", http->GetStatusCode());
            throw std::runtime_error("Upload failed");
        }
        std::string result = http->ReadAll();
        http->Close();
        ESP_LOGI(TAG, "JPEG upload: %d bytes, result=%s", current_fb_->len, result.c_str());
        return result;
    }

    // Non-JPEG fallback: encode and upload (original path)
    QueueHandle_t jpeg_queue = xQueueCreate(40, sizeof(JpegChunk));
    if (jpeg_queue == nullptr) throw std::runtime_error("Failed to create JPEG queue");

    encoder_thread_ = std::thread([this, jpeg_queue]() {
        int64_t start_time = esp_timer_get_time();
        uint16_t w = current_fb_->width, h = current_fb_->height;
        v4l2_pix_fmt_t enc_fmt = V4L2_PIX_FMT_RGB565;
        uint8_t *src = current_fb_->buf;
        size_t src_len = current_fb_->len;
        image_to_jpeg_cb(src, src_len, w, h, enc_fmt, 80,
            [](void* arg, size_t index, const void* data, size_t len) -> size_t {
                auto q = static_cast<QueueHandle_t>(arg);
                JpegChunk c = {.data = nullptr, .len = len};
                if (index == 0 && data && len > 0) {
                    c.data = (uint8_t*)heap_caps_aligned_alloc(16, len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                    if (c.data) memcpy(c.data, data, len); else c.len = 0;
                } else c.len = 0;
                xQueueSend(q, &c, portMAX_DELAY);
                return len;
            }, jpeg_queue);
        int64_t end_time = esp_timer_get_time();
        ESP_LOGI(TAG, "JPEG encoding: %ld ms", (long)((end_time - start_time) / 1000));
    });

    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(3);
    std::string boundary = "----ESP32_CAMERA_BOUNDARY";
    http->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    http->SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str());
    if (!explain_token_.empty())
        http->SetHeader("Authorization", "Bearer " + explain_token_);
    http->SetHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
    http->SetHeader("Transfer-Encoding", "chunked");
    if (!http->Open("POST", explain_url_)) {
        encoder_thread_.join();
        while (true) { JpegChunk c; if (xQueueReceive(jpeg_queue, &c, portMAX_DELAY) != pdPASS || c.data == nullptr) break; heap_caps_free(c.data); }
        vQueueDelete(jpeg_queue);
        throw std::runtime_error("Failed to connect");
    }

    {
        std::string question_field;
        question_field += "--" + boundary + "\r\n";
        question_field += "Content-Disposition: form-data; name=\"question\"\r\n";
        question_field += "\r\n";
        question_field += question + "\r\n";
        http->Write(question_field.c_str(), question_field.size());
    }
    {
        std::string file_header;
        file_header += "--" + boundary + "\r\n";
        file_header += "Content-Disposition: form-data; name=\"file\"; filename=\"camera.jpg\"\r\n";
        file_header += "Content-Type: image/jpeg\r\n";
        file_header += "\r\n";
        http->Write(file_header.c_str(), file_header.size());
    }

    size_t total_sent = 0;
    bool saw_terminator = false;
    std::vector<uint8_t> jpeg_buffer;
    while (true) {
        JpegChunk chunk;
        if (xQueueReceive(jpeg_queue, &chunk, portMAX_DELAY) != pdPASS) {
            ESP_LOGE(TAG, "Failed to receive JPEG chunk");
            break;
        }
        if (chunk.data == nullptr) {
            saw_terminator = true;
            break;
        }
        jpeg_buffer.insert(jpeg_buffer.end(), chunk.data, chunk.data + chunk.len);
        http->Write((const char *)chunk.data, chunk.len);
        total_sent += chunk.len;
        heap_caps_free(chunk.data);
    }
    encoder_thread_.join();
    vQueueDelete(jpeg_queue);

    if (!saw_terminator || total_sent == 0) {
        ESP_LOGE(TAG, "JPEG encoder failed or produced empty output");
        throw std::runtime_error("Failed to encode image to JPEG");
    }

    // Save to SD card (organized: /sdcard/photos/2026-07/20260729_131902.jpg)
    if (!jpeg_buffer.empty()) {
        time_t now = time(nullptr);
        struct tm timeinfo;
        localtime_r(&now, &timeinfo);
        char dir[48], filename[32], fullpath[128];
        strftime(dir, sizeof(dir), "/sdcard/photos/%Y-%m", &timeinfo);
        strftime(filename, sizeof(filename), "%Y%m%d_%H%M%S.jpg", &timeinfo);
        mkdir("/sdcard/photos", 0755);
        mkdir(dir, 0755);
        snprintf(fullpath, sizeof(fullpath), "%s/%s", dir, filename);
        FILE* fp = fopen(fullpath, "wb");
        if (fp) {
            fwrite(jpeg_buffer.data(), 1, jpeg_buffer.size(), fp);
            fclose(fp);
            ESP_LOGI(TAG, "Photo saved to SD: %s (%zu bytes)", fullpath, jpeg_buffer.size());
        } else {
            ESP_LOGW(TAG, "Failed to save photo to SD: %s", fullpath);
        }
    }

    {
        std::string multipart_footer;
        multipart_footer += "\r\n--" + boundary + "--\r\n";
        http->Write(multipart_footer.c_str(), multipart_footer.size());
    }
    http->Write("", 0);

    if (http->GetStatusCode() != 200) {
        ESP_LOGE(TAG, "Failed to upload photo, status code: %d", http->GetStatusCode());
        throw std::runtime_error("Failed to upload photo");
    }

    std::string result = http->ReadAll();
    http->Close();

    size_t remain_stack_size = uxTaskGetStackHighWaterMark(nullptr);
    ESP_LOGI(TAG, "Explain image size=%dx%d, compressed size=%d, remain stack size=%d, question=%s\n%s",
             current_fb_->width, current_fb_->height, (int)total_sent, (int)remain_stack_size, question.c_str(), result.c_str());
    return result;
}
