// Input Buffer — tick-based input delay + smart early-click buffering
// Target: Geode SDK v4.x, Geometry Dash 2.2074
//
// F6 = cycle buffer size (0 / 2 / 4 / 8 ticks)
// F7 = toggle mode (Fixed Delay <-> Smart Buffer)
//
// NOTE: GD 2.2 runs physics at 240 ticks/sec, so 8 ticks ~= 33 ms.

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <Geode/ui/Notification.hpp>

#include <array>
#include <deque>

using namespace geode::prelude;

namespace {

enum class Mode { Fixed, Smart };

constexpr std::array<int, 4> kDelays{0, 2, 4, 8};
size_t g_delayIdx = 0;
Mode g_mode = Mode::Fixed;

struct BufferedInput {
    bool down;
    int button;
    bool isPlayer1;
    uint64_t fireTick;         // fire no later than this tick
    bool waitForGround;        // smart mode: fire early once player lands
    uint64_t groundSeenTick = 0;
};

std::deque<BufferedInput> g_queue;
uint64_t g_tick = 0;
bool g_replaying = false;     // recursion guard for re-injected inputs

int currentDelay() { return kDelays[g_delayIdx]; }

const char* modeName() { return g_mode == Mode::Fixed ? "Fixed Delay" : "Smart Buffer"; }

PlayerObject* playerFor(GJBaseGameLayer* layer, bool isPlayer1) {
    return isPlayer1 ? layer->m_player1 : layer->m_player2;
}

// Flying gamemodes respond to holds continuously, so buffering makes no sense there.
bool isFlyingMode(PlayerObject* p) {
    return p->m_isShip || p->m_isBird || p->m_isDart || p->m_isSwing;
}

void fire(GJBaseGameLayer* layer, BufferedInput const& ev) {
    g_replaying = true;
    layer->handleButton(ev.down, ev.button, ev.isPlayer1);
    g_replaying = false;
}

// Fire queued events in order. Stops at the first event that isn't ready,
// so a release can never overtake its press.
void flushQueue(GJBaseGameLayer* layer, bool force = false) {
    while (!g_queue.empty()) {
        auto& ev = g_queue.front();
        bool ready = force || g_tick >= ev.fireTick;

        if (!ready && ev.waitForGround) {
            auto* p = playerFor(layer, ev.isPlayer1);
            if (p && p->m_isOnGround) {
                // Wait one tick after touching ground so the held input
                // gets consumed as a landing jump before we release it.
                if (ev.groundSeenTick == 0) ev.groundSeenTick = g_tick;
                else if (g_tick > ev.groundSeenTick) ready = true;
            }
        }

        if (!ready) break;
        BufferedInput copy = ev;
        g_queue.pop_front();
        fire(layer, copy);
    }
}

void notify(std::string const& msg) {
    Notification::create(msg, NotificationIcon::Info, 1.0f)->show();
}

} // namespace

class $modify(BufferedGameLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        int delay = currentDelay();

        // Pass through: replays, buffer off, or not in a real level (e.g. the editor).
        if (g_replaying || delay == 0 || !typeinfo_cast<PlayLayer*>(this)) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        if (g_mode == Mode::Fixed) {
            g_queue.push_back({down, button, isPlayer1, g_tick + delay, false});
            return;
        }

        // ---- Smart Buffer mode ----
        auto* player = playerFor(this, isPlayer1);

        if (down) {
            // A new press cancels any pending buffered release on this input,
            // otherwise the player would end up "released" while holding.
            flushQueue(this, true);
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        // Release: if the player is airborne in a ground-based gamemode,
        // keep the input held until landing (or until the window expires).
        if (player && !isFlyingMode(player) && !player->m_isOnGround) {
            g_queue.push_back({down, button, isPlayer1, g_tick + delay, true});
            return;
        }

        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    // Called once per physics tick in 2.2. If your bindings version uses a
    // different signature, check GJBaseGameLayer in geode-sdk/bindings and match it.
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        if (typeinfo_cast<PlayLayer*>(this)) {
            ++g_tick;
            flushQueue(this);
        }
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }
};

class $modify(BufferedPlayLayer, PlayLayer) {
    void resetLevel() {
        g_queue.clear();
        g_tick = 0;
        PlayLayer::resetLevel();
    }

    void onQuit() {
        g_queue.clear();
        PlayLayer::onQuit();
    }
};

class $modify(BufferKeys, CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool isKeyDown, bool isKeyRepeat, double timestamp) {
        if (isKeyDown && !isKeyRepeat && (key == KEY_F6 || key == KEY_F7)) {
            // Release anything pending before changing settings so no input gets stuck.
            if (auto* pl = PlayLayer::get()) flushQueue(pl, true);
            else g_queue.clear();

            if (key == KEY_F6) {
                g_delayIdx = (g_delayIdx + 1) % kDelays.size();
            } else {
                g_mode = (g_mode == Mode::Fixed) ? Mode::Smart : Mode::Fixed;
            }

            notify(fmt::format("{}: {} ticks", modeName(), currentDelay()));
            return true; // swallow the key
        }
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, isKeyDown, isKeyRepeat);
    }
};
