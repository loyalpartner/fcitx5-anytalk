#include "ipc_client.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cstring>
#include <cstdlib>
#include <string>
#include <json-c/json.h>
#include <fcitx-utils/log.h>

static std::string anytalk_socket_path() {
  const char *runtime_dir = std::getenv("XDG_RUNTIME_DIR");
  if (runtime_dir && *runtime_dir) {
    return std::string(runtime_dir) + "/anytalk.sock";
  }
  const char *uid = std::getenv("UID");
  if (uid && *uid) {
    return std::string("/run/user/") + uid + "/anytalk.sock";
  }
  return "/tmp/anytalk.sock";
}

IpcClient::IpcClient() = default;

void IpcClient::setCallbacks(TextCb partial, TextCb final, TextCb status) {
  on_partial_ = std::move(partial);
  on_final_ = std::move(final);
  on_status_ = std::move(status);
}

void IpcClient::start() {
  if (running_) return;
  running_ = true;
  recv_thread_ = std::thread(&IpcClient::recvLoop, this);
}

void IpcClient::stop() {
  running_ = false;
  if (sock_ >= 0) {
    ::shutdown(sock_, SHUT_RDWR);
    ::close(sock_);
    sock_ = -1;
  }
  if (recv_thread_.joinable()) {
    recv_thread_.join();
  }
}

void IpcClient::connectSocket() {
  if (sock_ >= 0) return;

  sock_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (sock_ < 0) {
    FCITX_ERROR() << "Failed to create socket";
    return;
  }

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::string path = anytalk_socket_path();
  std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());

  FCITX_DEBUG() << "Connecting to " << path;

  if (::connect(sock_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
    ::close(sock_);
    sock_ = -1;
  } else {
    FCITX_DEBUG() << "Connected to " << path;
  }
}

void IpcClient::sendJson(const std::string &json) {
  if (sock_ < 0) {
    FCITX_ERROR() << "Socket not connected, cannot send: " << json;
    return;
  }
  FCITX_DEBUG() << "Sending JSON: " << json;
  ::send(sock_, json.data(), json.size(), 0);
  ::send(sock_, "\n", 1, 0);
}

void IpcClient::sendStart() {
  sendJson("{\"type\":\"start\",\"mode\":\"toggle\"}");
}

void IpcClient::sendStop() {
  sendJson("{\"type\":\"stop\"}");
}

void IpcClient::sendCancel() {
  sendJson("{\"type\":\"cancel\"}");
}

void IpcClient::recvLoop() {
  while (running_) {
    if (sock_ < 0) {
      connectSocket();
      if (sock_ < 0) {
        ::usleep(200 * 1000);
        continue;
      }
    }

    char buf[4096];
    ssize_t n = ::recv(sock_, buf, sizeof(buf) - 1, 0);
    if (n <= 0) {
      ::close(sock_);
      sock_ = -1;
      if (on_status_) {
          on_status_("idle");
      }
      continue;
    }
    buf[n] = '\0';

    char *saveptr = nullptr;
    char *line = ::strtok_r(buf, "\n", &saveptr);
    while (line) {
      json_object *obj = json_tokener_parse(line);
      if (obj) {
        json_object *type_obj = nullptr;
        if (json_object_object_get_ex(obj, "type", &type_obj)) {
          const char *type = json_object_get_string(type_obj);
          if (type && std::strcmp(type, "partial") == 0) {
            json_object *text_obj = nullptr;
            if (json_object_object_get_ex(obj, "text", &text_obj) && on_partial_) {
              on_partial_(json_object_get_string(text_obj));
            }
          } else if (type && std::strcmp(type, "final") == 0) {
            json_object *text_obj = nullptr;
            if (json_object_object_get_ex(obj, "text", &text_obj) && on_final_) {
              on_final_(json_object_get_string(text_obj));
            }
          } else if (type && std::strcmp(type, "status") == 0) {
            json_object *state_obj = nullptr;
            if (json_object_object_get_ex(obj, "state", &state_obj) && on_status_) {
              on_status_(json_object_get_string(state_obj));
            }
          }
        }
        json_object_put(obj);
      }
      line = ::strtok_r(nullptr, "\n", &saveptr);
    }
  }
}
