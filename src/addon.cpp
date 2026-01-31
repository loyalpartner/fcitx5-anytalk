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
#include <regex>
#include <fcitx/userinterface.h>
#include <fcntl.h>

// --- AnyTalkStatusAction Implementation ---

AnyTalkStatusAction::AnyTalkStatusAction(AnyTalkEngine *engine) : engine_(engine) {
}

std::string AnyTalkStatusAction::shortText(fcitx::InputContext *ic) const {
    if (engine_->isRecording()) {
        return "REC";
    }
    std::string state = engine_->connectionState();
    if (state == "connecting") {
        return "...";
    }
    if (state == "connected") {
        return "RDY";
    }
    return "AT";
}

std::string AnyTalkStatusAction::icon(fcitx::InputContext *ic) const {
    if (engine_->isRecording()) {
        return "media-record"; // Use system record icon or "anytalk-rec"
    }
    return "anytalk";
}

// --- AnyTalkEngine Implementation ---

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

  statusAction_ = std::make_unique<AnyTalkStatusAction>(this);
  
  reloadConfig();
  startDaemon();
}

AnyTalkEngine::~AnyTalkEngine() {
  ipc_.stop();
}

// Helper to trigger UI refresh
void AnyTalkEngine::setStatus(const std::string &state) {
  current_state_ = state;
  if (state == "idle") {
    recording_ = false;
    active_ic_ = nullptr;
  } else if (state == "connected") {
    recording_ = false;
  } else if (state == "recording") {
    recording_ = true;
  }
  
  if (instance_) {
     auto *ic = instance_->inputContextManager().lastFocusedInputContext();
     if (ic) {
         statusAction_->update(ic);
         ic->updateUserInterface(fcitx::UserInterfaceComponent::StatusArea);
         // Also update input panel if needed, but StatusArea covers icons/labels
     }
  }
}

// V2 Overrides for Main Icon/Label
std::string AnyTalkEngine::subModeIconImpl(const fcitx::InputMethodEntry &, fcitx::InputContext &) {
    if (recording_) return "media-record";
    return "anytalk";
}

std::string AnyTalkEngine::subModeLabelImpl(const fcitx::InputMethodEntry &, fcitx::InputContext &) {
    if (recording_) return "REC";
    if (current_state_ == "connecting") return "...";
    if (current_state_ == "connected") return "RDY";
    return "AT";
}

void AnyTalkEngine::startDaemon() {
    if (*config_.developerMode) {
        FCITX_INFO() << "Developer mode enabled, skipping daemon auto-start.";
        return;
    }
    
    pid_t pid = fork();
    if (pid == 0) {
        // Child process
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        
        setenv("ANYTALK_APP_ID", config_.appId->c_str(), 1);
        setenv("ANYTALK_ACCESS_TOKEN", config_.accessToken->c_str(), 1);
        setenv("ANYTALK_RESOURCE_ID", "volc.seedasr.sauc.duration", 0);
        // Ensure RUST_LOG is set so tracing defaults to info if not specified
        setenv("RUST_LOG", "info", 0);
        
        std::string path = *config_.daemonPath;
        if (path.empty()) path = "anytalk-daemon";

        execlp(path.c_str(), path.c_str(), nullptr);
        
        // Only reached if execlp fails
        // We can't log easily here since we removed redirection, 
        // but fcitx might capture stderr or it goes to journal.
        _exit(1);
    } else if (pid > 0) {
        FCITX_INFO() << "Started anytalk-daemon with PID: " << pid;
    } else {
        FCITX_ERROR() << "Fork failed: " << strerror(errno);
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
    
    // Refresh UI on activate
    statusAction_->update(ic);
}

void AnyTalkEngine::deactivate(const fcitx::InputMethodEntry &, fcitx::InputContextEvent &event) {
    if (event.inputContext() == active_ic_) {
        active_ic_ = nullptr;
    }
}

void AnyTalkEngine::keyEvent(const fcitx::InputMethodEntry &, fcitx::KeyEvent &event) {
  if (event.isRelease()) {
    return;
  }

  auto *ic = event.inputContext();

  // Enter key
  if (event.key().sym() == FcitxKey_Return && recording_) {
      FCITX_DEBUG() << "Enter pressed, stopping recording";
      ipc_.sendStop();
      recording_ = false;
      
      // Update UI immediately
      setStatus("idle"); // This triggers refresh
      event.accept();
      return;
  }

  // F2 or Media Play key
  if (event.key().sym() == FcitxKey_F2 || event.key().sym() == FcitxKey_AudioPlay) {
    if (!recording_) {
      active_ic_ = ic;
      ignore_next_commit_ = false;
      ipc_.sendStart();
      if (current_state_ != "connected") {
          setStatus("connecting"); 
      }
    } else {
      ipc_.sendStop();
      setStatus("idle"); // Optimistic update
    }
    event.accept();
    return;
  }
}

void AnyTalkEngine::updatePreedit(const std::string &text) {
  if (!instance_) return;
  
  last_text_ = text;
  auto *focused = instance_->inputContextManager().lastFocusedInputContext();
  auto *ic = active_ic_ ? (active_ic_ == focused ? active_ic_ : nullptr) : focused;
  if (!ic) return;
  fcitx::Text preedit(text);
  ic->inputPanel().setClientPreedit(preedit);
  ic->updatePreedit();
}

void AnyTalkEngine::commitText(const std::string &text) {
  if (ignore_next_commit_) return;
  if (!instance_) return;
  last_text_ = "";
  auto *focused = instance_->inputContextManager().lastFocusedInputContext();
  auto *ic = active_ic_ ? (active_ic_ == focused ? active_ic_ : nullptr) : focused;
  if (!ic) return;
  ic->commitString(text);
  ic->inputPanel().setClientPreedit(fcitx::Text());
  ic->updatePreedit();
}

class AnyTalkFactory : public fcitx::AddonFactory {
public:
  fcitx::AddonInstance *create(fcitx::AddonManager *manager) override {
    return new AnyTalkEngine(manager ? manager->instance() : nullptr);
  }
};

FCITX_ADDON_FACTORY(AnyTalkFactory)
