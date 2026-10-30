// Used by desktop_fault_policy_test.py, which inserts actual production bodies.
// Transport effects are counted here; native protocol/app behavior is separate.
#include "drag_geometry.hpp"
#include "foreground_route.hpp"
#include "input_grant.hpp"
#include "passive_pointer_target.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace cua::hyprland;
using Clock = InputGrant::Clock;
struct Window { bool m_isX11 = false; };
struct Surface {
    bool m_mapped = true;
    int client() const { return 1; }
    bool good() const { return true; }
};
using Target = PassivePointerTarget<std::weak_ptr<Window>, std::weak_ptr<Surface>>;
constexpr int WL_POINTER_BUTTON_STATE_RELEASED = 0, WL_KEYBOARD_KEY_STATE_RELEASED = 0;
// A primary-seat pointer or keyboard the background primary route borrows.
struct Borrowed {
    unsigned releases = 0, leaves = 0;
    bool good() const { return true; }
    void sendButton(unsigned, unsigned, int state) { releases += state == WL_POINTER_BUTTON_STATE_RELEASED; }
    void sendKey(unsigned, unsigned, int) {}
    void sendMods(unsigned, unsigned, unsigned, unsigned) {}
    void sendLeave() { ++leaves; }
    void sendFrame() {}
};
struct SeatManager { struct { std::weak_ptr<Surface> pointerFocus, keyboardFocus; } m_state; } seat_manager;
SeatManager* g_pSeatManager = &seat_manager;
struct xkb_keymap;
struct xkb_state;
void xkb_state_unref(xkb_state*) {}
xkb_state* xkb_state_new(xkb_keymap*) { return nullptr; }
unsigned event_ms() { return 0; }
struct Client {
    bool dead = false;
    void* source = nullptr;
    int fd = -1;
    std::string token = "old-target";
    std::weak_ptr<Window> window;
    std::weak_ptr<Surface> surface;
    uint64_t approved_deadline = 42, revision = 1;
    InputRoute route = InputRoute::independent;
};
struct Pointer {
    bool dead = false, resource_live = true;
    Pointer* wl = this;
    std::weak_ptr<Surface> focus;
    bool resource() const { return resource_live; }
};
struct Drag { Client* client; DragGeometry geometry{1}; };
struct Trace { void mark(const char*, unsigned) {} };
void wl_event_source_remove(void*) {}
int close(int) { return 0; }
std::string refusal(std::string_view reason) { return std::string(reason); }
std::string refusal(std::string_view reason, ForegroundFailure) { return refusal(reason); }
void check(bool value, const char* message) {
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}

struct Lane {
    static constexpr bool kProduction = true;
    bool suspended = false, session = true, layout = true, target_live = true;
    bool primary_busy = false, peer_busy = false, refresh_ok = true;
    bool foreground_started = false, foreground_needs_keyboard = false, keyboard_focus = true;
    unsigned lane = 0, held_button = 272, releases = 0, leaves = 0;
    uint64_t capabilities = 8, desktop_generation = 1;
    Trace* trace = nullptr;
    void* listen_source = nullptr;
    int listener = -1;
    std::shared_ptr<Window> window = std::make_shared<Window>();
    std::shared_ptr<Surface> surface = std::make_shared<Surface>();
    Target::Geometry geometry{20, 30, 500, 400, 20, 30};
    Target pointer_target;
    std::vector<std::unique_ptr<Pointer>> pointers;
    std::vector<std::unique_ptr<Client>> clients;
    Client* lease = nullptr;
    Client* reservation = nullptr;
    std::optional<Drag> drag;
    InputGrant grant;
    Clock::time_point expires;
    std::vector<unsigned> held_keys{42};
    std::vector<std::string> responses;
    std::vector<std::weak_ptr<Borrowed>> borrowed_pointers, borrowed_keyboards, held_keyboards;
    std::weak_ptr<Surface> borrowed_surface, borrowed_seat, held_surface;
    xkb_state* physical_keyboard_state = nullptr;
    xkb_keymap* physical_keymap = nullptr;

    Lane() {
        clients.push_back(std::make_unique<Client>());
        lease = reservation = clients.front().get();
        lease->window = window; lease->surface = surface;
        pointer_target.capture(window, surface, geometry);
        pointers.push_back(std::make_unique<Pointer>());
        pointers.front()->focus = surface;
        grant.arm(8, Clock::now()); expires = grant.deadline();
        drag.emplace(lease);
    }
    bool available() const { return !suspended && session; }
    bool layout_qualified() const { return layout; }
    bool refresh(Client&) const { return refresh_ok; }
    template<typename T> bool primary_conflict(const T&) const { return primary_busy; }
    template<typename T> bool agent_conflict(const T&) const { return peer_busy; }
    std::optional<Target::Geometry> target_geometry(
        const std::shared_ptr<Window>& w, const std::shared_ptr<Surface>& s, bool = false) const {
        if (!target_live || !w || !s) return {};
        return geometry;
    }
    void send(Client&, const std::string& response) { responses.push_back(response); }
    void finish_foreground() { foreground_started = false; }
    void release_pointer_button() { if (held_button) ++releases; held_button = 0; }
    void leave_pointer() {
        release_pointer_button(); ++leaves; pointer_target.reset();
        for (auto& pointer : pointers) pointer->focus.reset();
    }
    void leave_keyboard() { held_keys.clear(); keyboard_focus = false; }
    void cleanup_socket() {}
    void make_layout_sensitive() {
        lease->route = InputRoute::primary_foreground;
        capabilities = 2;
        foreground_needs_keyboard = true;
        grant.arm(2, Clock::now());
        expires = grant.deadline();
    }
    // A primary-route drag holds its press on the target's own primary-seat
    // pointer. The agent seat has no focus there.
    std::shared_ptr<Borrowed> use_primary_route() {
        auto borrowed = std::make_shared<Borrowed>();
        lease->route = InputRoute::primary_background;
        pointer_target.reset();
        pointers.front()->focus.reset();
        borrowed_pointers.push_back(borrowed);
        borrowed_surface = borrowed_seat = surface;
        return borrowed;
    }
    // The primary keyboard an earlier agent click or key left entered.
    std::shared_ptr<Borrowed> hold_primary_route_keyboard() {
        auto held = std::make_shared<Borrowed>();
        held_keyboards.push_back(held);
        held_surface = surface;
        return held;
    }
    // PRODUCTION_METHODS
    void check_inert(bool retained = true) const {
        check(!lease && !drag && !held_button && held_keys.empty() && !keyboard_focus &&
              !capabilities && grant.deadline() == Clock::time_point{} &&
              expires == Clock::time_point{}, "fault retained input or action authority");
        check(releases == 1, "held button was not released exactly once");
        check(pointer_target.entered() == retained && leaves == (retained ? 0u : 1u),
              "unexpected pointer leave/retention");
    }
};

int main() {
    { Lane l; l.suspend(); l.guard_targets(); l.check_inert();
      check(!l.reservation && l.clients.empty() && !l.available(), "disable retained admission"); }
    { Lane l; l.suspend("plugin_shutdown"); l.guard_targets(); l.check_inert(false); }
    { Lane l; l.desktop_transition(); l.session = false; l.layout = false;
      l.guard_targets(); l.check_inert();
      check(!l.reservation && l.clients.front()->dead && l.desktop_generation == 2,
            "transition retained connection or generation");
      l.session = true; l.layout = true; l.guard_targets(); l.check_inert(); }
    { Lane l; l.desktop_generation = UINT64_MAX; l.desktop_transition();
      l.guard_targets(); l.check_inert(); check(l.suspended, "generation overflow admitted input"); }
    // A killed or stopped owner releases its drag and keeps the unchanged hover.
    { Lane l; l.lease->dead = true; l.guard_targets(); l.check_inert(); }
    { Lane l; l.cancel_authority("stopped"); l.guard_targets(); l.check_inert(); }
    // The primary route releases on the borrowed pointer, never on the agent seat.
    // A primary keyboard held between actions is left exactly once too.
    for (int path = 0; path < 4; ++path) {
        Lane l;
        const auto borrowed = l.use_primary_route();
        const auto held = l.hold_primary_route_keyboard();
        if (path == 0) l.lease->dead = true;
        if (path == 1) l.cancel_authority("stopped");
        if (path == 2) l.desktop_transition();
        if (path == 3) l.refresh_ok = false;
        l.guard_targets();
        check(borrowed->releases == 1 && borrowed->leaves == 1 && !l.releases && !l.held_button &&
              l.borrowed_pointers.empty(), "borrowed press was not released exactly once on its own route");
        check(held->leaves == 1 && l.held_keyboards.empty() && !l.held_surface.lock(),
              "held primary keyboard was not left exactly once");
    }
    // Once the person's own keyboard focus is in that app, the compositor
    // leaves it, not the agent.
    {
        Lane l;
        l.use_primary_route();
        const auto held = l.hold_primary_route_keyboard();
        seat_manager.m_state.keyboardFocus = l.surface;
        l.cancel_authority("stopped");
        seat_manager.m_state.keyboardFocus.reset();
        check(!held->leaves && l.held_keyboards.empty(), "agent left a keyboard the person's focus owns");
    }
    for (bool session_fault : {true, false}) {
        for (int path = 0; path < 3; ++path) {
            Lane l;
            if (session_fault) l.session = false; else { l.layout = false; l.make_layout_sensitive(); }
            if (path == 0) l.guard_targets();
            if (path == 1) l.target_refusal(*l.lease);
            if (path == 2) l.action_refusal(*l.lease);
            l.guard_targets(); l.check_inert();
            if (path == 1)
                check(l.clients.front()->token.empty() && !l.clients.front()->approved_deadline,
                      "refused TARGET retained old binding authority");
            l.session = true; l.layout = true; l.guard_targets(); l.check_inert();
        }
    }
    // Every passive safety guard still applies during either desktop fault.
    for (bool session_fault : {true, false}) {
        for (int bad = 0; bad < 7; ++bad) {
            Lane l; l.desktop_transition();
            if (session_fault) l.session = false; else l.layout = false;
            switch (bad) {
                case 0: l.target_live = false; break;
                case 1: ++l.geometry[0]; break;
                case 2: l.pointers.front()->dead = true; break;
                case 3: l.pointers.front()->resource_live = false; break;
                case 4: l.pointers.front()->focus.reset(); break;
                case 5: l.peer_busy = true; break;
                case 6: l.window.reset(); break;
            }
            l.guard_targets(); l.check_inert(false);
        }
        // The agent seat's resting pointer may share an app with the human.
        Lane l; l.desktop_transition();
        if (session_fault) l.session = false; else l.layout = false;
        l.primary_busy = true;
        l.guard_targets(); l.check_inert();
    }
    // Hover may rest on one of the target's popups and goes when it unmaps.
    {
        Lane l; l.desktop_transition();
        const auto popup = std::make_shared<Surface>();
        l.pointers.front()->focus = popup;
        l.guard_targets(); l.check_inert();
        popup->m_mapped = false;
        l.guard_targets(); l.check_inert(false);
    }
    // Active safety failures and explicit human takeover continue to leave.
    for (int bad = 0; bad < 6; ++bad) {
        Lane l;
        switch (bad) {
            case 0: l.refresh_ok = false; break;
            case 1: l.primary_busy = true; break;
            case 2: l.peer_busy = true; break;
            case 3: ++l.lease->revision; break;
            case 4: l.expires = Clock::time_point{}; break;
            case 5: l.cancel_authority("primary_target_busy", false); break;
        }
        l.guard_targets(); l.check_inert(false);
    }
    std::cout << "production desktop fault policy tests passed\n";
}
