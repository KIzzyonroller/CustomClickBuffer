#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>

using namespace geode::prelude;

namespace {
    // A release that we are holding back for a few frames.
    struct PendingRelease {
        bool active = false;
        float timeLeft = 0.f; // seconds
    };

    PendingRelease g_p1;
    PendingRelease g_p2;

    constexpr int JUMP_BUTTON = 1; // 1 = jump, 2 = left, 3 = right

    bool modeAllowed(PlayerObject* p) {
        auto mod = Mod::get();
        if (!p) return false;
        if (p->m_isShip)   return mod->getSettingValue<bool>("mode-ship");
        if (p->m_isBird)   return mod->getSettingValue<bool>("mode-ufo");
        if (p->m_isBall)   return mod->getSettingValue<bool>("mode-ball");
        if (p->m_isDart)   return mod->getSettingValue<bool>("mode-wave");
        if (p->m_isRobot)  return mod->getSettingValue<bool>("mode-robot");
        if (p->m_isSpider) return mod->getSettingValue<bool>("mode-spider");
        if (p->m_isSwing)  return mod->getSettingValue<bool>("mode-swing");
        return mod->getSettingValue<bool>("mode-cube");
    }

    float bufferSeconds() {
        int frames = static_cast<int>(Mod::get()->getSettingValue<int64_t>("frames"));
        frames = std::clamp(frames, 1, 5);
        // one "frame" = one rendered frame at the player's current FPS
        return frames * static_cast<float>(CCDirector::get()->getAnimationInterval());
    }
}

class $modify(BufferGJBGL, GJBaseGameLayer) {

    void handleButton(bool down, int button, bool isPlayer1) {
        auto mod = Mod::get();

        // Only touch the jump button, and only when the mod is on.
        if (!mod->getSettingValue<bool>("enabled") || button != JUMP_BUTTON) {
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }
        if (!isPlayer1 && !mod->getSettingValue<bool>("player2")) {
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }

        auto& pending = isPlayer1 ? g_p1 : g_p2;
        auto* player = isPlayer1 ? m_player1 : m_player2;

        if (down) {
            // New press while a release is still waiting: let the old one
            // finish first so this registers as a fresh click.
            if (pending.active) {
                pending.active = false;
                GJBaseGameLayer::handleButton(false, button, isPlayer1);
            }
            return GJBaseGameLayer::handleButton(true, button, isPlayer1);
        }

        // Release: delay it if this gamemode is enabled.
        if (!modeAllowed(player)) {
            return GJBaseGameLayer::handleButton(false, button, isPlayer1);
        }
        if (pending.active) return; // already waiting

        pending.active = true;
        pending.timeLeft = bufferSeconds();
    }

    void processCommands(float dt) {
        auto tick = [&](PendingRelease& pending, bool isPlayer1) {
            if (!pending.active) return;
            pending.timeLeft -= dt;
            if (pending.timeLeft <= 0.f) {
                pending.active = false;
                // Qualified call = original function, so we don't re-buffer.
                GJBaseGameLayer::handleButton(false, JUMP_BUTTON, isPlayer1);
            }
        };
        tick(g_p1, true);
        tick(g_p2, false);

        GJBaseGameLayer::processCommands(dt);
    }
};

// Don't carry a held-back release across a respawn or leaving the level.
class $modify(BufferPlayLayer, PlayLayer) {
    void resetLevel() {
        g_p1 = {};
        g_p2 = {};
        PlayLayer::resetLevel();
    }

    void onQuit() {
        g_p1 = {};
        g_p2 = {};
        PlayLayer::onQuit();
    }
};
