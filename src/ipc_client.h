#pragma once
#include <functional>
#include <string>
#include <thread>
#include <atomic>

class IpcClient {
public:
  using TextCb = std::function<void(const std::string &)>;

  IpcClient();
  void start();
  void stop();

  void sendStart();
  void sendStop();
  void sendCancel();

  void setCallbacks(TextCb partial, TextCb final, TextCb status);

private:
  void connectSocket();
  void recvLoop();
  void sendJson(const std::string &json);

  int sock_{-1};
  std::thread recv_thread_;
  std::atomic<bool> running_{false};

  TextCb on_partial_;
  TextCb on_final_;
  TextCb on_status_;
};
