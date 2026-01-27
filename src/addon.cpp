#include "addon.h"
#include <fcitx/addonfactory.h>
#include <fcitx/addonmanager.h>
#include <fcitx/inputpanel.h>
#include <fcitx/text.h>
#include <fcitx-utils/log.h>
#include <fcitx/statusarea.h>

AnyTalkEngine::AnyTalkEngine(fcitx::Instance *instance)
  : instance_(instance) {
  ipc_.setCallbacks(
    [this](const std::string &text) {
      if (!instance_) return;
      instance_->eventDispatcher().schedule([this, text]() {
        updatePreedit(text);
      });
    },
    [this](const std::string &text) {
      if (!instance_) return;
      instance_->eventDispatcher().schedule([this, text]() {
        commitText(text);
      });
    },
    [this](const std::string &state) {
      if (!instance_) return;
      instance_->eventDispatcher().schedule([this, state]() {
        setStatus(state);
      });
    }
  );
  ipc_.start();

  statusAction_ = std::make_unique<fcitx::SimpleAction>();
  statusAction_->setShortText("AT");
  statusAction_->setIcon("anytalk");
}

AnyTalkEngine::~AnyTalkEngine() {
  ipc_.stop();
}

void AnyTalkEngine::activate(const fcitx::InputMethodEntry &, fcitx::InputContextEvent &event) {
    auto *ic = event.inputContext();
    if (!ic) return;
    ic->statusArea().addAction(fcitx::StatusGroup::InputMethod, statusAction_.get());
    updateStatusItem(ic);
}

void AnyTalkEngine::deactivate(const fcitx::InputMethodEntry &, fcitx::InputContextEvent &event) {
    // StatusArea is managed by InputContext, typically cleared on focus change.
    // If we need to explicitly remove:
    // auto *ic = event.inputContext();
    // if (ic) ic->statusArea().removeAction(statusAction_.get());
}

void AnyTalkEngine::keyEvent(const fcitx::InputMethodEntry &, fcitx::KeyEvent &event) {
  if (event.isRelease()) {
    return;
  }

  if (event.key().sym() == FcitxKey_F2) {
    auto *ic = event.inputContext();
    if (!recording_) {
      FCITX_DEBUG() << "F2 pressed, sending start";
      ipc_.sendStart();
      recording_ = true;
    } else {
      FCITX_DEBUG() << "F2 pressed, sending stop";
      ipc_.sendStop();
      recording_ = false;
    }
    updateStatusItem(ic);
    event.accept();
    return;
  }
}

void AnyTalkEngine::updatePreedit(const std::string &text) {
  if (!instance_) {
    return;
  }
  last_text_ = text;
  auto *ic = instance_->inputContextManager().lastFocusedInputContext();
  if (!ic) {
    return;
  }
  fcitx::Text preedit(text);
  ic->inputPanel().setClientPreedit(preedit);
  ic->updatePreedit();
}

void AnyTalkEngine::commitText(const std::string &text) {
  if (!instance_) {
    return;
  }
  last_text_ = "";
  auto *ic = instance_->inputContextManager().lastFocusedInputContext();
  if (!ic) {
    return;
  }
  ic->commitString(text);
  ic->inputPanel().setClientPreedit(fcitx::Text());
  ic->updatePreedit();
}

void AnyTalkEngine::setStatus(const std::string &state) {
  current_state_ = state;
  if (state == "idle") {
    recording_ = false;
  } else if (state == "recording") {
    recording_ = true;
  }
  
  if (instance_) {
     auto *ic = instance_->inputContextManager().lastFocusedInputContext();
     if (ic) {
         updateStatusItem(ic);
     }
  }
}

void AnyTalkEngine::updateStatusItem(fcitx::InputContext *ic) {
  if (!statusAction_ || !ic) {
    return;
  }

  if (recording_ || current_state_ == "recording") {
    statusAction_->setShortText("REC");
    statusAction_->setIcon("anytalk-rec"); // 录音状态图标
  } else if (current_state_ == "connecting") {
    statusAction_->setShortText("...");
    statusAction_->setIcon("anytalk");
  } else {
    statusAction_->setShortText("AT");
    statusAction_->setIcon("anytalk"); // 空闲状态图标
  }
  statusAction_->update(ic);
}

class AnyTalkFactory : public fcitx::AddonFactory {
public:
  fcitx::AddonInstance *create(fcitx::AddonManager *manager) override {
    return new AnyTalkEngine(manager ? manager->instance() : nullptr);
  }
};

FCITX_ADDON_FACTORY(AnyTalkFactory)