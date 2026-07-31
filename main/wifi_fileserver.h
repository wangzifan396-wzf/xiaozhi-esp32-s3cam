#pragma once
#include <cstdio>
#include <string>
#include <dirent.h>
#include <sys/stat.h>
#include <esp_log.h>
#include <esp_http_server.h>
#include <esp_netif.h>
#define TAG_WFS "WiFiFS"

// Match all URIs — custom match function for the HTTP server
static bool match_all(const char* ref, const char* uri, size_t len) { return true; }

class WifiFileServer {
    static httpd_handle_t server_;

    static std::string urlDecode(std::string s) {
        for (size_t i=0; i<s.size(); i++)
            if (s[i]=='%' && i+2<s.size()) { int c; sscanf(s.c_str()+i+1,"%2x",&c); s.replace(i,3,1,(char)c); }
        return s;
    }

    static std::string uploadForm(const std::string& dir) {
        return "<form method='POST' enctype='multipart/form-data' style='margin:10px 0;padding:10px;background:#e8f5e9;border-radius:4px'>"
            "<input type='file' name='file' style='padding:4px'>"
            "<input type='hidden' name='dir' value='" + dir + "'>"
            "<button style='padding:4px 12px;margin-left:8px'>📤 上传</button></form>"
            "<form method='POST' enctype='multipart/form-data' style='display:inline;margin:10px 0;padding:10px;background:#fff3e0;border-radius:4px'>"
            "📁 新建文件夹: <input name='newfolder' style='padding:4px;width:120px' placeholder='文件夹名'>"
            "<input type='hidden' name='dir' value='" + dir + "'>"
            "<button style='padding:4px 12px;margin-left:8px'>创建</button></form>";
    }

    static esp_err_t Handler(httpd_req_t* req) {
        std::string uri = urlDecode(req->uri);

        // Handle POST (file upload or new folder)
        if (req->method == HTTP_POST) {
            char buf[512]; int ret, remaining = req->content_len;
            std::string body;
            while (remaining > 0) {
                ret = httpd_req_recv(req, buf, std::min((int)sizeof(buf), remaining));
                if (ret <= 0) break;
                body.append(buf, ret); remaining -= ret;
            }
            // Extract target directory
            auto getField = [&](const char* name) -> std::string {
                auto p = body.find(std::string("name=\"") + name + "\"");
                if (p == std::string::npos) return "";
                p = body.find("\r\n\r\n", p) + 4;
                auto e = body.find("\r\n---", p);
                if (e == std::string::npos) e = body.size() - 2;
                return body.substr(p, e - p);
            };
            std::string dir = getField("dir");
            if (dir.empty()) dir = "/sdcard";

            // New folder?
            std::string folderName = getField("newfolder");
            if (!folderName.empty()) {
                std::string path = dir + "/" + folderName;
                mkdir(path.c_str(), 0755);
                httpd_resp_set_status(req, "303 See Other");
                httpd_resp_set_hdr(req, "Location", dir.c_str());
                httpd_resp_send(req, nullptr, 0);
                return ESP_OK;
            }

            // File upload
            auto fn = body.find("filename=\"");
            if (fn == std::string::npos || body.find("filename=\"\"", fn) != std::string::npos) {
                httpd_resp_set_status(req, "303 See Other");
                httpd_resp_set_hdr(req, "Location", dir.c_str());
                httpd_resp_send(req, nullptr, 0);
                return ESP_OK;
            }
            fn += 10;
            auto fnEnd = body.find('"', fn);
            std::string filename = body.substr(fn, fnEnd - fn);
            auto dataStart = body.find("\r\n\r\n", fnEnd) + 4;
            auto dataEnd = body.rfind("\r\n---");
            if (dataEnd == std::string::npos) dataEnd = body.size();
            std::string data = body.substr(dataStart, dataEnd - dataStart);
            std::string path = dir + "/" + filename;
            FILE* fp = fopen(path.c_str(), "wb");
            if (fp) { fwrite(data.data(), 1, data.size(), fp); fclose(fp); }
            httpd_resp_set_status(req, "303 See Other");
            httpd_resp_set_hdr(req, "Location", dir.c_str());
            httpd_resp_send(req, nullptr, 0);
            return ESP_OK;
        }

        // Handle deletion: ?delete in query string
        auto delPos = uri.find("?delete");
        if (delPos != std::string::npos) {
            std::string delPath = uri.substr(0, delPos);
            struct stat st;
            if (stat(delPath.c_str(), &st) == 0) {
                if (S_ISDIR(st.st_mode)) {
                    // Recursively delete directory
                    auto rmdir_r = [](auto&& self, const char* p) -> void {
                        DIR* d = opendir(p); if (!d) return;
                        struct dirent* e;
                        while ((e = readdir(d))) {
                            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
                            std::string f = std::string(p) + "/" + e->d_name;
                            struct stat s;
                            if (stat(f.c_str(), &s) != 0) continue;
                            if (S_ISDIR(s.st_mode)) self(self, f.c_str());
                            else remove(f.c_str());
                        }
                        closedir(d);
                        rmdir(p);
                    };
                    rmdir_r(rmdir_r, delPath.c_str());
                } else {
                    remove(delPath.c_str());
                }
            }
            // Redirect to parent directory
            auto slash = delPath.rfind('/');
            std::string parent = (slash != std::string::npos) ? delPath.substr(0, slash) : "/";
            httpd_resp_set_status(req, "303 See Other");
            httpd_resp_set_hdr(req, "Location", parent.c_str());
            httpd_resp_send(req, nullptr, 0);
            return ESP_OK;
        }

        // GET handler (existing code)
        // Root page
        if (uri == "/") {
            auto dl = [](const char* p) {
                std::string h; DIR* d=opendir(p); if(!d) return h;
                h+="<h3>"+std::string(p)+"</h3><ul>";
                struct dirent* e;
                while((e=readdir(d))){ if(e->d_name[0]=='.')continue;
                    std::string f=std::string(p)+"/"+e->d_name; struct stat st; if(stat(f.c_str(),&st)!=0)continue;
                    h+="<li>"+std::string(S_ISDIR(st.st_mode)?"📁":"📄")+" <a href='"+f+"'>"+e->d_name+"</a>";
                    if(!S_ISDIR(st.st_mode)) h+=" <small>("+std::to_string(st.st_size/1024)+"KB)</small> <a href='"+f+"?download' style='display:inline;background:0;padding:0;color:#06c'>⬇</a>";
                    h+=" <a href='"+f+"?delete' style='display:inline;background:0;padding:0;color:red' onclick='return confirm(\"删除 "+std::string(e->d_name)+"?\")'>🗑</a>";
                    h+="</li>"; }
                closedir(d); h+="</ul>"; return h;
            };
            std::string page="<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>小智SD卡</title><style>body{font-family:sans-serif;max-width:800px;margin:0 auto;padding:10px}a{display:block;padding:6px;margin:2px 0;background:#f5f5f5;border-radius:4px;text-decoration:none;color:#333}small{color:#999}h3{color:#666}</style></head><body><h1>小智 SD 卡文件</h1><p><a href='/sdcard/photos'>📸 照片</a> <a href='/sdcard/chats'>💬 聊天记录</a> <a href='/sdcard/memos'>📝 备忘录</a></p>"+uploadForm("/sdcard")+dl("/sdcard/photos")+dl("/sdcard/chats")+dl("/sdcard/memos")+"</body></html>";
            httpd_resp_set_type(req,"text/html; charset=utf-8");
            httpd_resp_send(req,page.c_str(),page.size());
            return ESP_OK;
        }
        // Directory listing
        struct stat st;
        if (stat(uri.c_str(),&st)==0 && S_ISDIR(st.st_mode)) {
            std::string h="<!DOCTYPE html><html><head><meta charset='utf-8'><title>"+uri+"</title><style>body{font-family:sans-serif;max-width:800px;margin:0 auto;padding:10px}a{display:block;padding:6px;margin:2px 0;background:#f5f5f5;border-radius:4px;text-decoration:none;color:#333}</style></head><body><p><a href='/'>← 返回</a></p>"+uploadForm(uri)+"<h3>"+uri+"</h3><ul>";
            DIR* d=opendir(uri.c_str());
            if(d){ struct dirent* e; while((e=readdir(d))){ if(e->d_name[0]=='.')continue;
                std::string f=uri+"/"+e->d_name; struct stat st2;
                if(stat(f.c_str(),&st2)!=0)continue;
                h+="<li>"+std::string(S_ISDIR(st2.st_mode)?"📁":"📄")+" <a href='"+f+"'>"+e->d_name+"</a>";
                if(!S_ISDIR(st2.st_mode)) h+=" <small>("+std::to_string(st2.st_size/1024)+"KB)</small> <a href='"+f+"?download' style='display:inline;background:0;padding:0;color:#06c'>⬇</a>";
                h+=" <a href='"+f+"?delete' style='display:inline;background:0;padding:0;color:red' onclick='return confirm(\"删除 "+std::string(e->d_name)+"?\")'>🗑</a>";
                h+="</li>"; } closedir(d); }
            h+="</ul></body></html>";
            httpd_resp_set_type(req,"text/html; charset=utf-8");
            httpd_resp_send(req,h.c_str(),h.size());
            return ESP_OK;
        }
        // Check for ?download to force attachment
        bool download = uri.find("?download") != std::string::npos;
        auto qpos = uri.find('?');
        std::string path = (qpos != std::string::npos) ? uri.substr(0, qpos) : uri;

        // File download
        FILE* fp=fopen(path.c_str(),"rb"); if(!fp){ httpd_resp_send_404(req); return ESP_FAIL; }
        fseek(fp,0,SEEK_END); size_t size=ftell(fp); fseek(fp,0,SEEK_SET);
        char* data=(char*)malloc(size);
        if(data){ fread(data,1,size,fp);
            if (download) {
                httpd_resp_set_type(req, "application/octet-stream");
                auto slash = path.rfind('/');
                std::string fname = (slash != std::string::npos) ? path.substr(slash + 1) : path;
                httpd_resp_set_hdr(req, "Content-Disposition", ("attachment; filename=\"" + fname + "\"").c_str());
            } else {
                if(path.find(".jpg")!=std::string::npos) httpd_resp_set_type(req,"image/jpeg");
                else if(path.find(".log")!=std::string::npos||path.find(".txt")!=std::string::npos) httpd_resp_set_type(req,"text/plain; charset=utf-8");
                else httpd_resp_set_type(req,"application/octet-stream");
            }
            httpd_resp_send(req,data,size); free(data); }
        fclose(fp); return ESP_OK;
    }

public:
    static void Start() {
        httpd_config_t c = HTTPD_DEFAULT_CONFIG();
        c.max_uri_handlers = 4;
        c.server_port = 8080;
        c.uri_match_fn = match_all;
        c.stack_size = 3072;           // Minimal stack (default 4096)
        c.task_priority = 1;           // Lowest priority, never preempt audio
        if (httpd_start(&server_, &c) == ESP_OK) {
            httpd_uri_t h = {.uri="/", .method=HTTP_GET, .handler=Handler, .user_ctx=nullptr};
            httpd_register_uri_handler(server_, &h);
            httpd_uri_t hp = {.uri="/", .method=HTTP_POST, .handler=Handler, .user_ctx=nullptr};
            httpd_register_uri_handler(server_, &hp);
            ESP_LOGI(TAG_WFS, "File server on port 8080");
            esp_netif_t* n=esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            if(n){ esp_netif_ip_info_t ip; if(esp_netif_get_ip_info(n,&ip)==ESP_OK) ESP_LOGI(TAG_WFS, "http://" IPSTR ":8080", IP2STR(&ip.ip)); }
        }
    }
};
httpd_handle_t WifiFileServer::server_ = nullptr;
