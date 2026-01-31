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

class AnyTalkEngine;

class AnyTalkStatusAction : public fcitx::Action {
public:
    AnyTalkStatusAction(AnyTalkEngine *engine);
    std::string shortText(fcitx::InputContext *ic) const override;
    std::string icon(fcitx::InputContext *ic) const override;
    // We can also implement activate() here if clicking the icon should toggle recording!
private:
    AnyTalkEngine *engine_;
};

class AnyTalkEngine : public fcitx::InputMethodEngineV2 {
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
  void startDaemon();
  
  // V2 Overrides for Main Icon/Label
  std::string subModeIconImpl(const fcitx::InputMethodEntry &entry, fcitx::InputContext &ic) override;
  std::string subModeLabelImpl(const fcitx::InputMethodEntry &entry, fcitx::InputContext &ic) override;

  // Accessors for StatusAction
  bool isRecording() const { return recording_; }
  std::string connectionState() const { return current_state_; }

private:
  fcitx::Instance *instance_;
  IpcClient ipc_;
  AnyTalkConfig config_;
  bool recording_{false};
  bool ignore_next_commit_{false};
  std::string last_text_;
  std::string current_state_{"idle"};
  fcitx::InputContext *active_ic_{nullptr};
  std::unique_ptr<AnyTalkStatusAction> statusAction_;
};
