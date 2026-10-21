#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>

using namespace geode::prelude;

namespace {
    constexpr int JUMP_BUTTON = 1; // 1 = jump, 2 = left, 3 = right

    // ---- release buffer (delays letting go) ----
    struct PendingRelease {
        bool active = false;
        float timeLeft = 0.f; // seconds
    };
    PendingRelease g_p1;
    PendingRelease g_p2;

    // ---- predictive press (holds a click back until it is safe) ----
    struct PendingPress {
        bool requested = false; // a click is being held back
        bool scheduled = false; // the delay has been decided
        float timeLeft = 0.f;   // seconds until we fire it
    };
    PendingPress g_press;

    // ---- simulation state ----
    bool g_simulating = false;
    bool g_simDied = false;
    float g_lastDt = 1.f / 240.f;
    bool g_lastHalfTick = false;
    bool g_lastLastTick = true;

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

    // One "frame" = one rendered frame at the player's current FPS.
    float frameSeconds() {
        return static_cast<float>(CCDirector::get()->getAnimationInterval());
    }

    int intSetting(char const* key, int lo, int hi) {
        return std::clamp(static_cast<int>(Mod::get()->getSettingValue<int64_t>(key)), lo, hi);
    }

    void clearAll() {
        g_p1 = {};
        g_p2 = {};
        g_press = {};
    }
}

class $modify(BufferGJBGL, GJBaseGameLayer) {

    // Send the held-back click to the game for real.
    void deliverPress() {
        g_press = {};
        GJBaseGameLayer::handleButton(true, JUMP_BUTTON, true);
    }

    // Play the near future with the click happening after `delayTicks`.
    // Returns true if the player survives the whole look-ahead.
    bool runSimulation(int delayTicks, int lookTicks, int holdTicks, float dt) {
        g_simDied = false;
        bool pressed = false;
        bool released = false;

        for (int t = 0; t < lookTicks; ++t) {
            if (!pressed && t >= delayTicks) {
                GJBaseGameLayer::handleButton(true, JUMP_BUTTON, true);
                pressed = true;
            }
            if (pressed && !released && t >= delayTicks + holdTicks) {
                GJBaseGameLayer::handleButton(false, JUMP_BUTTON, true);
                released = true;
            }
            GJBaseGameLayer::processCommands(dt, g_lastHalfTick, g_lastLastTick);
            if (g_simDied) return false;
        }
        return true;
    }

    // Decide how many ticks to delay the click. 0 = fire now.
    int chooseDelayTicks(float dt) {
        auto pl = PlayLayer::get();
        if (!pl || m_player1->m_isDead) return 0;

        float frame = frameSeconds();
        int ticksPerFrame = std::max(1, static_cast<int>(std::lround(frame / dt)));
        int maxDelay  = intSetting("pred-delay", 1, 5) * ticksPerFrame;
        int lookTicks = intSetting("pred-lookahead", 5, 60) * ticksPerFrame;
        int holdTicks = intSetting("pred-hold", 1, 30) * ticksPerFrame;
        int stride    = std::max(1, ticksPerFrame / 2);

        CheckpointObject* cp = pl->createCheckpoint();
        if (!cp) return 0;
        cp->retain();

        g_simulating = true;
        int chosen = 0;
        bool found = false;

        for (int d = 0; d <= maxDelay; d += stride) {
            if (d > 0) pl->loadFromCheckpoint(cp); // rewind before each retry
            if (runSimulation(d, lookTicks, holdTicks, dt)) {
                chosen = d;
                found = true;
                break; // earliest safe click wins
            }
        }

        pl->loadFromCheckpoint(cp); // put the real game back
        g_simulating = false;
        cp->release();

        // If no delay saves you, don't interfere: your click goes through as normal.
        return found ? chosen : 0;
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        // Our own simulation calls come straight through.
        if (g_simulating) {
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }

        auto mod = Mod::get();
        if (!mod->getSettingValue<bool>("enabled") || button != JUMP_BUTTON) {
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }
        if (!isPlayer1 && !mod->getSettingValue<bool>("player2")) {
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }

        auto& pending = isPlayer1 ? g_p1 : g_p2;
        auto* player = isPlayer1 ? m_player1 : m_player2;

        bool predictive = isPlayer1
            && mod->getSettingValue<bool>("predictive")
            && PlayLayer::get() != nullptr
            && modeAllowed(player);

        if (down) {
            // Finish any delayed release first so this is a fresh click.
            if (pending.active) {
                pending.active = false;
                GJBaseGameLayer::handleButton(false, button, isPlayer1);
            }

            if (predictive) {
                // A second click while one is still held back: let the first one out.
                if (g_press.requested) {
                    deliverPress();
                    GJBaseGameLayer::handleButton(false, button, isPlayer1);
                }
                // Hold this click back; chooseDelayTicks() runs at the next tick.
                g_press.requested = true;
                g_press.scheduled = false;
                return;
            }
            return GJBaseGameLayer::handleButton(true, button, isPlayer1);
        }

        // ---- release ----
        // If the click was still being held back, send it now so the release has something to end.
        if (isPlayer1 && g_press.requested) {
            deliverPress();
        }

        if (!modeAllowed(player)) {
            return GJBaseGameLayer::handleButton(false, button, isPlayer1);
        }
        if (pending.active) return;

        pending.active = true;
        pending.timeLeft = static_cast<float>(intSetting("frames", 1, 5)) * frameSeconds();
    }

    // Don't let the simulation eat the real queued inputs.
    void processQueuedButtons(float dt, bool clearInputQueue) {
        if (g_simulating) return;
        GJBaseGameLayer::processQueuedButtons(dt, clearInputQueue);
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        if (g_simulating) {
            return GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        }

        g_lastDt = dt;
        g_lastHalfTick = isHalfTick;
        g_lastLastTick = isLastTick;

        // Delayed releases
        auto tick = [&](PendingRelease& pending, bool isPlayer1) {
            if (!pending.active) return;
            pending.timeLeft -= dt;
            if (pending.timeLeft <= 0.f) {
                pending.active = false;
                GJBaseGameLayer::handleButton(false, JUMP_BUTTON, isPlayer1);
            }
        };
        tick(g_p1, true);
        tick(g_p2, false);

        // Held-back press
        if (g_press.requested) {
            if (!g_press.scheduled) {
                int delayTicks = chooseDelayTicks(dt);
                if (delayTicks <= 0) {
                    deliverPress();
                } else {
                    g_press.scheduled = true;
                    g_press.timeLeft = delayTicks * dt;
                }
            } else {
                g_press.timeLeft -= dt;
                if (g_press.timeLeft <= 0.f) deliverPress();
            }
        }

        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }
};

class $modify(BufferPlayLayer, PlayLayer) {
    // While simulating, a "death" is only a prediction, not a real death.
    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (g_simulating) {
            g_simDied = true;
            return;
        }
        PlayLayer::destroyPlayer(player, object);
    }

    // Don't let a simulated run finish the level for real.
    void levelComplete() {
        if (g_simulating) return;
        PlayLayer::levelComplete();
    }

    void resetLevel() {
        clearAll();
        PlayLayer::resetLevel();
    }

    void onQuit() {
        clearAll();
        PlayLayer::onQuit();
    }
};
