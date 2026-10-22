#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/ui/Notification.hpp>
#include <chrono>

using namespace geode::prelude;

// Diagnostic: proves the mod's code is running at all.
$on_mod(Loaded) {
    log::info("DIAG: Custom Click Buffer code loaded, v{}", Mod::get()->getVersion().toVString());
}

namespace {
    constexpr int JUMP_BUTTON = 1; // 1 = jump, 2 = left, 3 = right

    // ---- orb buffer ----
    // After you click, we watch for a NEW orb to be touched within the window.
    // If one is, we click for you on that exact step. We never hold your button
    // longer than you did, so landing can't cause an extra jump.
    struct OrbWindow {
        bool active = false;
        float total = 0.f;     // seconds, full window
        float timeLeft = 0.f;  // seconds
        bool waitForGround = false; // click was wasted in the air: fire on landing
        bool flag = true;           // the game's input flag to replay with
    };
    OrbWindow g_win1;
    OrbWindow g_win2;

    // A short release we send ourselves right after an orb click.
    struct PendingRelease {
        bool active = false;
        float timeLeft = 0.f; // seconds
        bool flag = true;     // the game's input flag to replay with
    };
    PendingRelease g_p1;
    PendingRelease g_p2;

    // ---- predictive press (holds a click back until it is safe) ----
    struct PendingPress {
        bool requested = false; // a click is being held back
        bool scheduled = false; // the delay has been decided
        float timeLeft = 0.f;   // seconds until we fire it
        float heldFor = 0.f;    // how long you've been holding this click
        bool releaseQueued = false; // you let go before we fired it
        float holdDuration = 0.f;   // how long you actually held it
        bool flag = true;           // the game's input flag to replay with
    };
    PendingPress g_press;

    // ---- what YOU are physically doing (player 1) ----
    bool g_userHolding = false;
    bool g_userFlag = false;
    bool g_swallowRepeats = false; // after a cancelled click, ignore repeats until you let go
    int g_repeatCount = 0;
    int g_stuckFixes = 0;

    // ---- simulation state ----
    bool g_simulating = false;
    bool g_skipVisuals = false;   // "Fast Tests": skip drawing-only work during test runs
    double g_lastTestMs = 0.0;    // how long the last click test took

    bool skipVisuals() { return g_simulating && g_skipVisuals; }
    bool g_simDied = false;
    float g_lastDt = 1.f / 240.f;
    bool g_lastHalfTick = false;
    bool g_lastLastTick = true;

    // Safety limits for the predictive mode
    constexpr int MAX_SIM_TICKS = 600;      // hard cap on simulated steps per click
    constexpr double MIN_SIM_GAP_MS = 40.0; // don't run tests more often than this
    bool g_budgetHit = false;
    std::chrono::steady_clock::time_point g_lastSim = {};

    // ---- counters shown on screen ----
    int g_orbCount = 0;
    int g_delayedCount = 0;
    int g_cancelCount = 0;
    int g_unsavableCount = 0;

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
        g_win1 = {};
        g_win2 = {};
        g_p1 = {};
        g_p2 = {};
        g_press = {};
    }

    // ---------------- on-screen feedback ----------------

    CCLabelBMFont* findLabel(std::string const& id) {
        auto pl = PlayLayer::get();
        if (!pl || !pl->m_uiLayer) return nullptr;
        return typeinfo_cast<CCLabelBMFont*>(pl->m_uiLayer->getChildByID(id));
    }

    void refreshStats() {
        auto label = findLabel("click-hud-stats"_spr);
        if (!label) return;

        auto mod = Mod::get();
        bool enabled = mod->getSettingValue<bool>("enabled");
        bool predictive = mod->getSettingValue<bool>("fix-early-clicks");

        label->setString(fmt::format(
            "{} {} | fix bad clicks {} | window {}f | buffered {} | delayed {} | cancelled {} | fail {} | unstuck {} | test {:.0f}ms",
            Mod::get()->getVersion().toVString(),
            enabled ? "ON" : "OFF",
            predictive ? "ON" : "OFF",
            intSetting("frames", 1, 5),
            g_orbCount, g_delayedCount, g_cancelCount, g_unsavableCount, g_stuckFixes, g_lastTestMs
        ).c_str());
    }

    // Flash a short message and log it.
    void showHud(std::string const& text, ccColor3B color) {
        if (g_simulating) return; // never report from inside a test run
        log::info("{}", text);

        if (!Mod::get()->getSettingValue<bool>("show-hud")) return;

        refreshStats();

        auto label = findLabel("click-hud-msg"_spr);
        if (!label) return;

        label->setString(text.c_str());
        label->setColor(color);
        label->stopAllActions();
        label->setOpacity(255);
        label->runAction(CCSequence::create(
            CCDelayTime::create(0.9f),
            CCFadeTo::create(0.35f, 0),
            nullptr
        ));
    }

    constexpr ccColor3B COL_ORB   = {80, 220, 255};
    constexpr ccColor3B COL_SAFE  = {255, 255, 255};
    constexpr ccColor3B COL_SAVED = {90, 255, 120};
    constexpr ccColor3B COL_FAIL  = {255, 90, 90};
}

class $modify(BufferGJBGL, GJBaseGameLayer) {

    // Open the orb-buffer window for a click that is really being applied.
    // Modes where a click only does something while on the ground.
    static bool isGroundMode(PlayerObject* p) {
        return !p->m_isShip && !p->m_isBird && !p->m_isDart && !p->m_isSwing;
        // = cube, ball, robot, spider
    }

    // Called with the game state from BEFORE the click is applied.
    // In a normal (non two-player) level, every click controls player 1,
    // whatever flag the game sends with it.
    bool controlsPlayer1(bool flag) {
        bool twoPlayer = m_levelSettings && m_levelSettings->m_twoPlayerMode;
        return flag || !twoPlayer;
    }

    void noteClick(bool slotP1, bool flag) {
        auto* player = slotP1 ? m_player1 : m_player2;
        if (!player || !modeAllowed(player)) return;

        auto& win = slotP1 ? g_win1 : g_win2;
        win.active = true;
        win.flag = flag;
        win.total = static_cast<float>(intSetting("frames", 1, 5)) * frameSeconds();
        win.timeLeft = win.total;

        // Only buffer a landing if this click is about to be wasted:
        // a ground mode, in the air, and not touching an orb.
        win.waitForGround = isGroundMode(player)
            && !player->m_isOnGround
            && !touchingUnusedOrb(player);
    }

    // Is the player overlapping an orb it hasn't used yet?
    static bool touchingUnusedOrb(PlayerObject* player) {
        if (!player || !player->m_touchingRings) return false;
        for (auto obj : CCArrayExt<GameObject*>(player->m_touchingRings)) {
            if (!obj) continue;
            if (!player->m_touchedRings.count(obj->m_uniqueID)) return true;
        }
        return false;
    }

    // Send the held-back click to the game for real.
    void deliverPress() {
        bool releaseQueued = g_press.releaseQueued;
        float holdDuration = g_press.holdDuration;
        bool flag = g_press.flag;
        g_press = {};
        noteClick(true, flag);
        GJBaseGameLayer::handleButton(true, JUMP_BUTTON, flag);

        // You already let go: release after the same hold time you used.
        if (releaseQueued) {
            g_p1.active = true;
            g_p1.timeLeft = std::max(holdDuration, 0.0001f);
            g_p1.flag = flag;
        }
    }

    // Play the near future with the click happening after `delayTicks`.
    // Returns true if the player survives the whole look-ahead.
    bool runSimulation(int delayTicks, int lookTicks, int holdTicks, float dt, int& budget) {
        g_simDied = false;
        bool pressed = false;
        bool released = false;

        for (int t = 0; t < lookTicks; ++t) {
            if (!pressed && t >= delayTicks) {
                GJBaseGameLayer::handleButton(true, JUMP_BUTTON, g_press.flag);
                pressed = true;
            }
            if (pressed && !released && t >= delayTicks + holdTicks) {
                GJBaseGameLayer::handleButton(false, JUMP_BUTTON, g_press.flag);
                released = true;
            }
            if (--budget < 0) {
                g_budgetHit = true;
                break; // out of budget: give up and don't interfere
            }
            // A full game step: inputs, movement, collisions, deaths.
            // (processCommands alone only applies inputs - the player never moved.)
            GJBaseGameLayer::update(dt);
            if (g_simDied) break;
        }
        // A test run must never leave the button pressed: the checkpoint
        // restore does NOT reset input, so a leftover press would stay held
        // in the real game and make you keep jumping.
        if (pressed && !released) {
            GJBaseGameLayer::handleButton(false, JUMP_BUTTON, g_press.flag);
        }
        if (g_budgetHit) return true;
        return !g_simDied;
    }

    // Decide how many ticks to delay the click. 0 = fire now.
    // foundOut = whether any tested delay survives. framesOut = chosen delay in frames
    // (-1 = test too heavy, -2 = skipped because clicks came too fast).
    int chooseDelayTicks(float dt, bool& foundOut, float& framesOut, bool& cancelOut) {
        foundOut = true;
        framesOut = 0.f;
        cancelOut = false;

        auto pl = PlayLayer::get();
        if (!pl || m_player1->m_isDead) return 0;

        // Don't run tests back-to-back (protects FPS if you spam clicks).
        auto now = std::chrono::steady_clock::now();
        double sinceLast = std::chrono::duration<double, std::milli>(now - g_lastSim).count();
        if (sinceLast < MIN_SIM_GAP_MS) {
            framesOut = -2.f;
            return 0;
        }
        g_lastSim = now;
        g_budgetHit = false;

        float frame = frameSeconds();
        // Delay window in physics steps (at least 1).
        int maxDelay  = std::max(1, static_cast<int>(std::lround(intSetting("pred-delay", 1, 5) * frame / dt)));
        int lookTicks = std::max(1, static_cast<int>(std::lround(intSetting("pred-look-ms", 100, 600) / 1000.f / dt)));
        int holdTicks = std::max(1, static_cast<int>(std::lround(intSetting("pred-hold-ms", 10, 300) / 1000.f / dt)));
        int stride    = std::max(1, maxDelay / 4); // at most ~5 tests per click

        // What to do with a click that would kill you.
        auto mode = Mod::get()->getSettingValue<std::string>("bad-click-mode");
        bool allowDelay  = mode != "Cancel only";
        bool allowCancel = mode != "Delay only";
        if (!allowDelay) maxDelay = 0; // only test the click as-is

        CheckpointObject* cp = pl->createCheckpoint();
        if (!cp) return 0;
        cp->retain();

        // update() changes these; the checkpoint doesn't restore them.
        double savedExtraDelta = m_extraDelta;
        int savedStep = m_currentStep;

        g_simulating = true;
        int chosen = 0;
        bool found = false;
        int budget = MAX_SIM_TICKS;

        for (int d = 0; d <= maxDelay; d += stride) {
            if (d > 0) {
                pl->loadFromCheckpoint(cp); // rewind before each retry
                m_extraDelta = savedExtraDelta;
                m_currentStep = savedStep;
            }
            if (runSimulation(d, lookTicks, holdTicks, dt, budget)) {
                chosen = d;
                found = true;
                break; // earliest safe click wins
            }
        }

        // Nothing worked: would you live if you hadn't clicked at all?
        bool cancelSafe = false;
        if (!found && allowCancel && !g_budgetHit) {
            pl->loadFromCheckpoint(cp);
            m_extraDelta = savedExtraDelta;
            m_currentStep = savedStep;
            // A delay longer than the test means the click never happens.
            cancelSafe = runSimulation(lookTicks + 1, lookTicks, holdTicks, dt, budget);
        }

        pl->loadFromCheckpoint(cp); // put the real game back
        m_extraDelta = savedExtraDelta;
        m_currentStep = savedStep;
        g_simulating = false;
        cp->release();

        log::info("DIAG: test result found={} chosenDelay={} cancelSafe={} budgetHit={} look={} steps",
            found, chosen, cancelSafe, g_budgetHit, lookTicks);

        foundOut = found;
        framesOut = static_cast<float>(chosen) * dt / std::max(frame, 0.0001f);

        if (g_budgetHit) {
            foundOut = true;
            framesOut = -1.f;
            return 0;
        }

        if (!found && cancelSafe) {
            cancelOut = true;
            foundOut = true;
            return 0;
        }

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
        bool slotP1 = controlsPlayer1(isPlayer1);

        // While you hold the button, the game keeps re-sending "pressed".
        // Those are NOT new clicks: never test or buffer them.
        if (slotP1 && down && g_userHolding && isPlayer1 == g_userFlag) {
            g_repeatCount++;
            if (g_swallowRepeats || g_press.requested) return; // click is cancelled / still held back
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }

        log::info("DIAG: click down={} button={} p1={} fixEarly={} (repeats so far {})",
            down, button, isPlayer1,
            mod->getSettingValue<bool>("fix-early-clicks"), g_repeatCount);

        if (slotP1) {
            if (down) { g_userHolding = true; g_userFlag = isPlayer1; }
            else if (isPlayer1 == g_userFlag) {
                g_userHolding = false;
                g_swallowRepeats = false;
            }
        }
        if (!slotP1 && !mod->getSettingValue<bool>("player2")) {
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }

        auto& pendingRel = slotP1 ? g_p1 : g_p2;
        auto* player = slotP1 ? m_player1 : m_player2;

        bool predictive = slotP1
            && mod->getSettingValue<bool>("fix-early-clicks")
            && PlayLayer::get() != nullptr
            && modeAllowed(player);

        if (down) {
            // If we still owe the game a short release from an orb click, send it first.
            if (pendingRel.active) {
                pendingRel.active = false;
                GJBaseGameLayer::handleButton(false, button, pendingRel.flag);
            }

            if (predictive) {
                // A second click while one is still held back: let the first one out.
                if (g_press.requested) {
                    bool oldFlag = g_press.flag;
                    g_press.releaseQueued = false;
                    deliverPress();
                    GJBaseGameLayer::handleButton(false, button, oldFlag);
                }
                // Hold this click back; chooseDelayTicks() runs at the next tick.
                g_press.requested = true;
                g_press.scheduled = false;
                g_press.flag = isPlayer1;
                return;
            }

            noteClick(slotP1, isPlayer1);
            return GJBaseGameLayer::handleButton(true, button, isPlayer1);
        }

        // ---- release: always passed straight through (never delayed) ----
        // (Only the release that matches the held-back click; the game also
        // sends stray releases with the other flag, which we pass through.)
        if (slotP1 && g_press.requested && isPlayer1 == g_press.flag) {
            // The click hasn't been sent yet. Remember the release and send
            // it after the delayed click, with the same hold length.
            g_press.releaseQueued = true;
            g_press.holdDuration = g_press.heldFor;
            return;
        }
        return GJBaseGameLayer::handleButton(false, button, isPlayer1);
    }

    // Decide what to do with a held-back click. Runs at the start of a
    // frame, OUTSIDE the game's step loop, so the test runs can use full
    // game steps safely.
    void decidePendingPress() {
        float dt = g_lastDt;
        bool found = true;
        float frames = 0.f;
        bool cancel = false;

        g_skipVisuals = Mod::get()->getSettingValue<bool>("fast-tests");
        auto t0 = std::chrono::steady_clock::now();
        int delayTicks = chooseDelayTicks(dt, found, frames, cancel);
        g_lastTestMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        log::info("DIAG: click test took {:.1f} ms", g_lastTestMs);

        if (cancel) {
            // Your click would have killed you: throw it away.
            // (Your release still reaches the game; releasing an
            // un-pressed button does nothing.)
            bool stillHolding = !g_press.releaseQueued;
            g_press = {};
            g_swallowRepeats = stillHolding;
            g_cancelCount++;
            showHud("CLICK CANCELLED - SAVED!", COL_SAVED);
        } else if (frames == -1.f) {
            showHud("TEST TOO HEAVY - SKIPPED", COL_FAIL);
        } else if (frames == -2.f) {
            // clicked again too quickly: pass through silently
        } else if (!found) {
            g_unsavableCount++;
            showHud("CAN'T SAVE THIS CLICK", COL_FAIL);
        } else if (delayTicks > 0) {
            g_delayedCount++;
            showHud(fmt::format("CLICK DELAYED {:.1f}f - SAVED!", frames), COL_SAVED);
        } else {
            showHud("CLICK SAFE", COL_SAFE);
        }

        if (cancel) {
            // nothing to send
        } else if (delayTicks <= 0) {
            deliverPress();
        } else {
            g_press.scheduled = true;
            g_press.timeLeft = delayTicks * dt;
        }
    }

    // Drawing-only work: pointless inside a test run.
    void updateDebugDraw() {
        if (skipVisuals()) return;
        GJBaseGameLayer::updateDebugDraw();
    }

    void updateAudioVisualizer() {
        if (skipVisuals()) return;
        GJBaseGameLayer::updateAudioVisualizer();
    }

    void update(float frameDt) {
        if (!g_simulating && g_press.requested && !g_press.scheduled) {
            decidePendingPress();
        }
        GJBaseGameLayer::update(frameDt);
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

        static bool loggedOnce = false;
        if (!loggedOnce) {
            loggedOnce = true;
            log::info("DIAG: processCommands hook is running (dt={})", dt);
        }

        g_lastDt = dt;
        g_lastHalfTick = isHalfTick;
        g_lastLastTick = isLastTick;

        // Safety net: if you are NOT holding the button and the mod isn't in
        // the middle of a click, the player must not be holding either.
        // This stops any stuck press (from a test run or anything else).
        if (Mod::get()->getSettingValue<bool>("enabled")
            && !g_userHolding && !g_press.requested && !g_p1.active) {
            auto heldDown = [](PlayerObject* p) {
                if (!p) return false;
                auto it = p->m_holdingButtons.find(JUMP_BUTTON);
                return it != p->m_holdingButtons.end() && it->second;
            };
            bool stuck = heldDown(m_player1)
                || (m_gameState.m_isDualMode && heldDown(m_player2)
                    && !(m_levelSettings && m_levelSettings->m_twoPlayerMode));
            if (stuck) {
                g_stuckFixes++;
                log::info("DIAG: stuck press detected, releasing (flag={}, fix #{})", g_userFlag, g_stuckFixes);
                GJBaseGameLayer::handleButton(false, JUMP_BUTTON, g_userFlag);
                if (heldDown(m_player1)) {
                    // Still held: try the other flag too.
                    GJBaseGameLayer::handleButton(false, JUMP_BUTTON, !g_userFlag);
                    log::info("DIAG: needed the other flag to release");
                }
            }
        }

        // Short releases we owe the game after an orb click
        auto tickRelease = [&](PendingRelease& pending) {
            if (!pending.active) return;
            pending.timeLeft -= dt;
            if (pending.timeLeft <= 0.f) {
                pending.active = false;
                GJBaseGameLayer::handleButton(false, JUMP_BUTTON, pending.flag);
            }
        };
        tickRelease(g_p1);
        tickRelease(g_p2);

        // Orb buffer: did you click shortly before touching a NEW orb?
        auto orbCheck = [&](OrbWindow& win, bool isPlayer1) {
            if (!win.active) return;
            auto* player = isPlayer1 ? m_player1 : m_player2;
            if (!player || player->m_isDead) { win.active = false; return; }

            win.timeLeft -= dt;
            if (win.timeLeft <= 0.f) { win.active = false; return; }

            bool orb = touchingUnusedOrb(player);
            bool landed = win.waitForGround && player->m_isOnGround;
            if (!orb && !landed) return;

            win.active = false; // one orb click per click

            // If you're still holding, the game activates the orb itself.
            auto it = player->m_holdingButtons.find(JUMP_BUTTON);
            bool holding = it != player->m_holdingButtons.end() && it->second;
            if (holding) return;

            float earlyFrames = (win.total - win.timeLeft) / std::max(frameSeconds(), 0.0001f);
            GJBaseGameLayer::handleButton(true, JUMP_BUTTON, win.flag);
            auto& rel = isPlayer1 ? g_p1 : g_p2;
            rel.active = true;
            rel.timeLeft = dt; // let go on the very next step
            rel.flag = win.flag;

            g_orbCount++;
            showHud(fmt::format("{} BUFFERED ({:.1f}f early)", orb ? "ORB" : "JUMP", earlyFrames), COL_ORB);
        };
        orbCheck(g_win1, true);
        orbCheck(g_win2, false);

        // Held-back press: count the hold, and fire a delayed click on time.
        // (The test that decides what to do runs in update(), at the start
        // of the frame - never inside this step.)
        if (g_press.requested) {
            if (!g_press.releaseQueued) g_press.heldFor += dt;
            if (g_press.scheduled) {
                g_press.timeLeft -= dt;
                if (g_press.timeLeft <= 0.f) deliverPress();
            }
        }

        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }
};

class $modify(BufferPlayLayer, PlayLayer) {
    // Build the on-screen readout once the level has loaded.
    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();

        log::info("DIAG: setupHasCompleted hook fired, uiLayer={}", m_uiLayer != nullptr);

        g_orbCount = 0;
        g_delayedCount = 0;
        g_cancelCount = 0;
        g_unsavableCount = 0;
        g_stuckFixes = 0;
        clearAll();

        if (!m_uiLayer) return;

        auto msg = CCLabelBMFont::create("", "bigFont.fnt");
        msg->setID("click-hud-msg"_spr);
        msg->setAnchorPoint({0.f, 0.f});
        msg->setPosition({8.f, 30.f});
        msg->setScale(0.35f);
        msg->setOpacity(0);
        m_uiLayer->addChild(msg, 100);

        auto stats = CCLabelBMFont::create("", "bigFont.fnt");
        stats->setID("click-hud-stats"_spr);
        stats->setAnchorPoint({0.f, 0.f});
        stats->setPosition({8.f, 8.f});
        stats->setScale(0.22f);
        stats->setOpacity(200);
        m_uiLayer->addChild(stats, 100);

        if (Mod::get()->getSettingValue<bool>("show-hud")) {
            refreshStats();
        } else {
            stats->setVisible(false);
        }
    }

    // ---- drawing / UI work skipped during test runs ("Fast Tests") ----
    void updateVisibility(float dt) {
        if (skipVisuals()) return;
        PlayLayer::updateVisibility(dt);
    }

    void updateProgressbar() {
        if (skipVisuals()) return;
        PlayLayer::updateProgressbar();
    }

    void updateInfoLabel() {
        if (skipVisuals()) return;
        PlayLayer::updateInfoLabel();
    }

    void updateTimeLabel(int seconds, int centiseconds, bool decimals) {
        if (skipVisuals()) return;
        PlayLayer::updateTimeLabel(seconds, centiseconds, decimals);
    }

    // Test runs shouldn't add to your attempt time.
    void updateAttemptTime(float attemptTime) {
        if (g_simulating) return;
        PlayLayer::updateAttemptTime(attemptTime);
    }

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
