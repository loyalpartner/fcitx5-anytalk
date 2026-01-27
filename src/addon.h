#pragma once
#include <fcitx/inputmethodengine.h>
#include <fcitx/instance.h>
#include <fcitx/inputcontext.h>
#include <fcitx/action.h>
#include "ipc_client.h"

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

  void updatePreedit(const std::string &text);
  void commitText(const std::string &text);
  void setStatus(const std::string &state);
  void updateStatusItem(fcitx::InputContext *ic);

private:
  fcitx::Instance *instance_;
  IpcClient ipc_;
  bool recording_{false};
  std::string last_text_;
  std::string current_state_{"idle"};
  std::unique_ptr<fcitx::SimpleAction> statusAction_;
};
