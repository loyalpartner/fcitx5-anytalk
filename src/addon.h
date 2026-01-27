#pragma once
#include <fcitx/inputmethodengine.h>
#include <fcitx/instance.h>
#include <fcitx/inputcontext.h>
#include <fcitx/action.h>
#include <fcitx-config/configuration.h>
#include <fcitx-config/iniparser.h>
#include "ipc_client.h"

FCITX_CONFIGURATION(
    AnyTalkConfig,
    fcitx::Option<std::string> appId{this, "AppID", "Volcengine App ID"};
    fcitx::Option<std::string> accessToken{this, "AccessToken", "Volcengine Access Token"};
    fcitx::Option<bool> developerMode{this, "DeveloperMode", "Developer Mode (Do not auto-start daemon)", false};
    fcitx::Option<std::string> daemonPath{this, "DaemonPath", "Path to anytalk-daemon executable", "/usr/bin/anytalk-daemon"};
);

class AnyTalkEngine : public fcitx::InputMethodEngine {
public:
  AnyTalkEngine(fcitx::Instance *instance);
  ~AnyTalkEngine();
  void activate(const fcitx::InputMethodEntry &entry,
                fcitx::InputContextEvent &event) override;
  void deactivate(const fcitx::InputMethodEntry &entry,
                  fcitx::InputContextEvent &event) override;
  void keyEvent(const fcitx::InputMethodEntry &entry,
                fcitx::KeyEvent &event) override;
  
  void setConfig(const fcitx::RawConfig &config) override;
  void reloadConfig() override;
  const fcitx::Configuration *getConfig() const override { return &config_; }

  void updatePreedit(const std::string &text);
  void commitText(const std::string &text);
  void setStatus(const std::string &state);
  void updateStatusItem(fcitx::InputContext *ic);
  void startDaemon();

private:
  fcitx::Instance *instance_;
  IpcClient ipc_;
  AnyTalkConfig config_;
  bool recording_{false};
  bool ignore_next_commit_{false};
  std::string last_text_;
  std::string current_state_{"idle"};
  std::unique_ptr<fcitx::SimpleAction> statusAction_;
};
