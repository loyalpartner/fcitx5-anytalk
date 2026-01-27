#include "addon.h"
#include <fcitx/addonfactory.h>
#include <fcitx/addonmanager.h>
#include <fcitx/inputpanel.h>
#include <fcitx/text.h>
#include <fcitx-utils/log.h>
#include <fcitx/statusarea.h>
#include <fcitx-utils/keysymgen.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <signal.h>

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
  
  reloadConfig();
  startDaemon();
}

AnyTalkEngine::~AnyTalkEngine() {
  ipc_.stop();
}

void AnyTalkEngine::startDaemon() {
    if (*config_.developerMode) {
        FCITX_INFO() << "Developer mode enabled, skipping daemon auto-start.";
        return;
    }
    
    // Check if daemon needs to be started
    pid_t pid = fork();
    if (pid == 0) {
        // Child process
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        
        setenv("ANYTALK_APP_ID", config_.appId->c_str(), 1);
        setenv("ANYTALK_ACCESS_TOKEN", config_.accessToken->c_str(), 1);
        setenv("ANYTALK_RESOURCE_ID", "volc.seedasr.sauc.duration", 0);
        
        std::string path = *config_.daemonPath;
        if (path.empty()) path = "anytalk-daemon"; // Fallback to PATH

        execlp(path.c_str(), path.c_str(), nullptr);
        
        FCITX_ERROR() << "Failed to exec anytalk-daemon at " << path;
        _exit(1);
    } else if (pid > 0) {
        FCITX_INFO() << "Started anytalk-daemon with PID " << pid;
    } else {
        FCITX_ERROR() << "Failed to fork anytalk-daemon";
    }
}

void AnyTalkEngine::setConfig(const fcitx::RawConfig &config) {
    config_.load(config, true);
    fcitx::safeSaveAsIni(config_, "conf/anytalk.conf");
}

void AnyTalkEngine::reloadConfig() {
    fcitx::readAsIni(config_, "conf/anytalk.conf");
}

void AnyTalkEngine::activate(const fcitx::InputMethodEntry &, fcitx::InputContextEvent &event) {
    auto *ic = event.inputContext();
    if (!ic) return;
    ic->statusArea().addAction(fcitx::StatusGroup::InputMethod, statusAction_.get());
    updateStatusItem(ic);
}

void AnyTalkEngine::deactivate(const fcitx::InputMethodEntry &, fcitx::InputContextEvent &event) {
    // StatusArea is managed by InputContext
}

void AnyTalkEngine::keyEvent(const fcitx::InputMethodEntry &, fcitx::KeyEvent &event) {
  if (event.isRelease()) {
    return;
  }

  // Enter key: Stop recording and commit immediately
  if (event.key().sym() == FcitxKey_Return && recording_) {
      auto *ic = event.inputContext();
      FCITX_DEBUG() << "Enter pressed, stopping recording";
      ipc_.sendStop();
      recording_ = false;
      updateStatusItem(ic);
      event.accept(); // Consume Enter so app doesn't get newline
      return;
  }

  // F2 or Media Play key: Toggle recording
  if (event.key().sym() == FcitxKey_F2 || event.key().sym() == FcitxKey_AudioPlay) {
    auto *ic = event.inputContext();
    if (!recording_) {
      FCITX_DEBUG() << "Trigger key pressed, sending start";
      ipc_.sendStart();
      recording_ = true;
    } else {
      FCITX_DEBUG() << "Trigger key pressed, sending stop";
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
    statusAction_->setIcon("anytalk-rec"); 
  } else if (current_state_ == "connecting") {
    statusAction_->setShortText("...");
    statusAction_->setIcon("anytalk");
  } else {
    statusAction_->setShortText("AT");
    statusAction_->setIcon("anytalk"); 
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
