// Independent-seat design adapted from Dillon DuPont's Hyprland prototype.
// TARGET uses independent resources; FOREGROUND_TARGET explicitly uses the primary seat.
#include "input_experiment.hpp"
#include "drag_geometry.hpp"
#include "input_grant.hpp"
#include "input_client_deadline.hpp"
#include "passive_pointer_target.hpp"
#include "primary_trace.hpp"
#include "seat_lifetime.hpp"
#include "owned_socket_path.hpp"
#include "foreground_route.hpp"
#include "keymap_equivalence.hpp"
#include "cursor_layer.hpp"
#include "target_identity.hpp"
#include "reaction_window.hpp"
#include "state_request.hpp"
#include "pointer_hit.hpp"

#include <src/Compositor.hpp>
#include <src/devices/IKeyboard.hpp>
#include <src/event/EventBus.hpp>
#include <src/managers/SeatManager.hpp>
#include <src/output/Monitor.hpp>
#include <src/protocols/core/Compositor.hpp>
#include <src/state/MonitorState.hpp>
#include <src/state/WorkspaceState.hpp>
#include <src/desktop/state/FocusState.hpp>
#include <src/desktop/view/Popup.hpp>
#include <src/managers/input/InputManager.hpp>
#include <src/managers/fullscreen/FullscreenController.hpp>
#include <src/managers/EventManager.hpp>
#include <src/pointer/PointerManager.hpp>
#include <src/protocols/core/Seat.hpp>
#include <src/protocols/core/DataDevice.hpp>
#include <src/layout/LayoutManager.hpp>
#include <src/plugins/PluginAPI.hpp>
#include <src/protocols/XDGShell.hpp>
#include <src/protocols/LayerShell.hpp>
#include <src/render/Renderer.hpp>
#include <src/render/pass/SurfacePassElement.hpp>
#include <src/xwayland/XSurface.hpp>
#include <linux/input-event-codes.h>
#include <wayland.hpp>
#include <wayland-server-core.h>

#ifdef CUA_HYPRLAND_TEST_INPUT
#include <openssl/evp.h>
#endif
#include <sys/random.h>
#include <sys/mman.h>
#include <xkbcommon/xkbcommon.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <format>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#if defined(CUA_HYPRLAND_INPUT) == defined(CUA_HYPRLAND_TEST_INPUT)
#error Select exactly one input admission mode
#endif

namespace cua::hyprland {
namespace {
using Clock = std::chrono::steady_clock;
constexpr std::size_t kMaxPacket = 2048;
constexpr std::size_t kMaxClients = 8;
constexpr std::size_t kMaxResources = 512;
// Clients react to a click within a few frames. Two seconds also covers a
// renderer process that is briefly descheduled, without leaving a refusal
// window long enough to block the human's own later use of that app.
constexpr auto kReactionWindow = std::chrono::seconds(2);
// A click or modifier chord the human gave the app of `surface`.
struct HumanCommand {
    WP<CWLSurfaceResource> surface;
    Clock::time_point at;
};
#ifdef CUA_HYPRLAND_INPUT
constexpr bool kProduction = true;
#else
constexpr bool kProduction = false;
#endif

#ifdef CUA_HYPRLAND_TEST_INPUT
std::uint64_t unix_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}
#endif
std::uint32_t event_ms() {
    return static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
               Clock::now().time_since_epoch()).count());
}
std::string nonce() {
    std::array<unsigned char, 16> bytes{};
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto n = getrandom(bytes.data() + offset, bytes.size() - offset, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) throw std::runtime_error("input entropy unavailable");
        offset += static_cast<std::size_t>(n);
    }
    std::string result;
    for (auto b : bytes)
        result += std::format("{:02x}", b);
    return result;
}
#ifdef CUA_HYPRLAND_TEST_INPUT
std::vector<unsigned char> unhex(std::string_view value) {
    if (value.size() % 2)
        throw std::runtime_error("invalid hex");
    std::vector<unsigned char> bytes;
    for (std::size_t i = 0; i < value.size(); i += 2) {
        unsigned int n = 0;
        const auto [p, error] = std::from_chars(value.data() + i, value.data() + i + 2, n, 16);
        if (error != std::errc{} || p != value.data() + i + 2)
            throw std::runtime_error("invalid hex");
        bytes.push_back(static_cast<unsigned char>(n));
    }
    return bytes;
}
#endif
std::uint64_t number(std::string_view value, int base = 10) {
    std::uint64_t n = 0;
    const auto [p, error] = std::from_chars(value.data(), value.data() + value.size(), n, base);
    if (value.empty() || error != std::errc{} || p != value.data() + value.size())
        throw std::runtime_error("invalid integer");
    return n;
}
double real(std::string_view value) {
    double n = 0;
    const auto [p, error] = std::from_chars(value.data(), value.data() + value.size(), n);
    if (value.empty() || error != std::errc{} || p != value.data() + value.size() || !std::isfinite(n))
        throw std::runtime_error("invalid coordinate");
    return n;
}
std::vector<std::string> fields(std::string_view packet) {
    if (packet.empty() || packet.front() == ' ' || packet.back() == ' ')
        throw std::runtime_error("invalid packet");
    std::vector<std::string> result;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= packet.size(); ++i) {
        if (i < packet.size() && (packet[i] < 32 || packet[i] > 126))
            throw std::runtime_error("non-ASCII packet");
        if (i != packet.size() && packet[i] != ' ')
            continue;
        if (i == start || result.size() >= 16)
            throw std::runtime_error("invalid fields");
        result.emplace_back(packet.substr(start, i - start));
        start = i + 1;
    }
    return result;
}
std::string refusal(std::string_view code) {
    // Only internal fixed identifiers enter JSON. No caller-provided strings.
    return std::format(R"({{"ok":false,"code":"{}","detail":"{}"}})", code, code);
}
std::string refusal(std::string_view code, ForegroundFailure failure) {
    return std::format(R"({{"ok":false,"code":"{}","detail":"{}"}})", code, failure.detail());
}
constexpr auto kDelivered = R"({"ok":true,"effect":"unverifiable","route":"synthetic_events"})";
constexpr auto kForegroundDelivered = R"({"ok":true,"effect":"unverifiable","route":"primary_foreground"})";
constexpr auto kPrimaryBackgroundDelivered = R"({"ok":true,"effect":"unverifiable","route":"primary_background"})";
// Locked, display off, another VT, or shutting down. cua:status reports this
// too, because Driver's AT-SPI actions never pass through a lane.
bool session_available() {
    return g_pCompositor->m_sessionActive && g_pCompositor->m_dpmsStateOn &&
        !g_pCompositor->m_isShuttingDown && !g_pSessionLockManager->isSessionLocked();
}
} // namespace

struct InputExperiment::Impl {
    struct Seat { SP<CWlSeat> wl; bool dead = false; };
    struct Pointer { SP<CWlPointer> wl; bool dead = false; WP<CWLSurfaceResource> focus; };
    struct Keyboard { SP<CWlKeyboard> wl; bool dead = false; WP<CWLSurfaceResource> focus; };
    struct Touch { SP<CWlTouch> wl; bool dead = false; };
    struct Client {
        Impl* owner = nullptr;
        int fd = -1;
        wl_event_source* source = nullptr;
        bool dead = false, hello = false;
        // Sticky for this connection: a bare CLAIM does not own inherited
        // hover. Never reset on STOP/CANCEL, repeated CLAIM, or invalidation.
        bool has_bound_target = false;
        InputRoute route = InputRoute::unbound;
        // Set only by XWAYLAND_TARGET. Driver attaches this lane's XWayland
        // devices to their own X master before asking, so stock XWayland
        // keeps agent input off the human's core pointer and keyboard.
        bool xwayland = false;
        // Human keyboard focus when XWAYLAND_TARGET admitted the target.
        WP<CWLSurfaceResource> human_focus;
        bool foreground_attempted = false;
        std::string challenge, token;
        WP<Desktop::View::CWindow> window;
        WP<CWLSurfaceResource> surface;
        std::array<double, 6> geometry{};
        std::uint64_t revision = 1, sequence = 0, approved_deadline = 0;
        InputClientDeadline deadline;
        CHyprSignalListener unmap, destroy;
        ~Client() {
            if (source) wl_event_source_remove(source);
            if (fd >= 0) close(fd);
        }
    };
    struct Drag {
        Client* client;
        double x1, y1, x2, y2;
        Clock::time_point start;
        unsigned duration;
        DragGeometry geometry;
    };
    int listener = -1;
    wl_event_source* listen_source = nullptr;
    wl_event_source* timer = nullptr;
    wl_global* global = nullptr;
    OwnedSocketPath socket_path;
    std::string_view socket_cleanup = "not_bound";
    std::string path, epoch = nonce(), keymap_text, physical_keymap_text;
    std::vector<unsigned char> public_key;
    std::vector<std::unique_ptr<Client>> clients;
    std::vector<std::unique_ptr<Seat>> seats;
    std::vector<std::unique_ptr<Pointer>> pointers;
    std::vector<std::unique_ptr<Keyboard>> keyboards;
    std::vector<std::unique_ptr<Touch>> touches;
    Client* lease = nullptr;
    // Pointer presence is not input authority. Keep the same live surface
    // entered between actions, as a real pointer is, without retaining a grant.
    PassivePointerTarget<WP<Desktop::View::CWindow>, WP<CWLSurfaceResource>> pointer_target;
    CHyprSignalListener pointer_unmap, pointer_destroy;
    Client* reservation = nullptr;
    std::uint64_t desktop_generation = 1;
    std::uint64_t capabilities = 0, dispatches = 0;
    Clock::time_point expires{};
    InputGrant grant;
    std::optional<Drag> drag;
    std::uint32_t held_button = 0;
    std::vector<std::uint32_t> held_keys;
    xkb_context* xkb_context_ = nullptr;
    xkb_keymap* keymap = nullptr;
    xkb_state* keyboard_state = nullptr;
    xkb_context* physical_xkb_context = nullptr;
    xkb_keymap* physical_keymap = nullptr;
    xkb_state* physical_keyboard_state = nullptr;
    int keymap_fd = -1;
    // Hyprland's input:repeat_rate and input:repeat_delay defaults, advertised
    // until an active keyboard reports the user's values.
    int repeat_rate = 25, repeat_delay = 600;
    bool retired = false, suspended = true, typing_keymap = false, physical_keymap_present = false;
    WP<IKeyboard> physical_keyboard;
    CHyprSignalListener keymap_listener;
    unsigned lane;
    std::array<Impl*, 2> peers{};
    PrimaryTrace* trace = nullptr;
    bool foreground_started = false, foreground_activating = false;
    bool foreground_keyboard_used = false;
    bool foreground_needs_keyboard = false;
    bool foreground_needs_pointer = false;
    WP<CWLSurfaceResource> foreground_surface;
    WP<CWLSeatResource> foreground_seat;
    std::vector<WP<CWLPointerResource>> foreground_pointers;
    std::vector<WP<CWLKeyboardResource>> foreground_keyboards;
    std::array<std::uint32_t, 4> foreground_modifiers{};
    // Primary-seat resources borrowed by the background primary route for one
    // action. The compositor's own focus never points at them.
    std::vector<WP<CWLPointerResource>> borrowed_pointers;
    std::vector<WP<CWLKeyboardResource>> borrowed_keyboards;
    WP<CWLSurfaceResource> borrowed_surface;
    WP<CWLSeatResource> borrowed_seat;
    // The last client this lane delivered to. A popup grab or drag start that
    // client sends shortly afterwards is a reaction to agent input.
    WP<CWLSurfaceResource> recent_target;
    // Borrowed primary-seat resources take their serials from Hyprland. They
    // only act in apps the human is not in, where the app rule applies.
    mutable AgentSerials agent_serials;
    // The app and workspace of the window this lane last delivered to, for a
    // window that app opens after closing it.
    struct DrivenApp {
        pid_t pid = 0;
        bool x = false;
        WORKSPACEID workspace = WORKSPACE_INVALID;
        std::string workspace_name;
        MONITORID monitor = MONITOR_INVALID;
    } recent_app;
    Clock::time_point recent_dispatch{};

    explicit Impl(const std::string& directory, unsigned index) : lane(index) {
#ifdef CUA_HYPRLAND_TEST_INPUT
        public_key = unhex(CUA_HYPRLAND_TEST_OPERATOR_KEY);
        if (public_key.size() != 32)
            throw std::runtime_error("invalid test operator public key");
#endif
        path = directory + (kProduction ?
            (lane == 0 ? "/cua-input-v3.sock" : "/cua-input-v3-2.sock") :
            (lane == 0 ? "/cua-input-test.sock" : "/cua-input-test-2.sock"));
        if (path.size() >= sizeof(sockaddr_un::sun_path))
            throw std::runtime_error("input socket path too long");
        // No private key or input-enabled default exists in this component.
    }

    static int create_keymap_file(std::string_view text) {
        const auto fd = memfd_create("cua-agent-keymap", MFD_CLOEXEC);
        std::string payload(text);
        payload.push_back('\0');
        if (fd < 0 || ftruncate(fd, static_cast<off_t>(payload.size())) != 0) {
            if (fd >= 0) close(fd);
            throw std::runtime_error("agent keymap file unavailable");
        }
        std::size_t offset = 0;
        while (offset < payload.size()) {
            const auto written = pwrite(fd, payload.data() + offset, payload.size() - offset, static_cast<off_t>(offset));
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) { close(fd); throw std::runtime_error("agent keymap write failed"); }
            offset += static_cast<std::size_t>(written);
        }
        return fd;
    }
    void initialize_agent_keymap() {
        if (keyboard_state) return;
        const xkb_rule_names names{kAgentKeymap.rules.data(), kAgentKeymap.model.data(),
            kAgentKeymap.layout.data(), kAgentKeymap.variant.data(), kAgentKeymap.options.data()};
        auto* context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        auto* map = context ? xkb_keymap_new_from_names(context, &names, XKB_KEYMAP_COMPILE_NO_FLAGS) : nullptr;
        auto* state = map ? xkb_state_new(map) : nullptr;
        char* serialized = map ? xkb_keymap_get_as_string(map, XKB_KEYMAP_FORMAT_TEXT_V1) : nullptr;
        if (!state || !serialized) {
            std::free(serialized);
            if (state) xkb_state_unref(state);
            if (map) xkb_keymap_unref(map);
            if (context) xkb_context_unref(context);
            throw std::runtime_error("agent US keymap unavailable");
        }
        std::string text(serialized);
        std::free(serialized);
        int fd = -1;
        try {
            fd = create_keymap_file(text);
        } catch (...) {
            xkb_state_unref(state);
            xkb_keymap_unref(map);
            xkb_context_unref(context);
            throw;
        }
        xkb_context_ = context;
        keymap = map;
        keyboard_state = state;
        keymap_text = std::move(text);
        keymap_fd = fd;
    }
    bool layout_qualified() const {
        if (!kProduction) return true;
        const auto keyboard = g_pSeatManager->m_keyboard.lock();
        return physical_keymap_present && typing_keymap && physical_keyboard_state && keyboard &&
            keyboard->m_xkbKeymapV1FD.get() >= 0 && keyboard->m_xkbKeymapV1String == physical_keymap_text;
    }
    // Agent keyboards advertise the user seat's wl_keyboard.repeat_info. A zero
    // rate is valid Wayland, but a single-seat client that binds an agent seat
    // may divide by it: imv 5.0.1 raised SIGFPE on repeat_info(0, 0) (#4257).
    //
    // A real rate cannot make Driver typing repeat. Protocol v3 has no key-down
    // or key-up request. One KEY request presses the key and its modifiers,
    // releases them, and leaves keyboard focus in a single synchronous handler
    // on the compositor thread, with no event-loop iteration in between. The
    // client gets the release together with the press and handles both in one
    // dispatch, so a repeat timer armed by the press is cancelled before the
    // client's event loop can run it; the wl_keyboard.leave that follows also
    // requires the client to stop repeating. The hold is therefore bounded by
    // that handler, not by the user's delay. DRAG holds only a pointer button.
    // A request that holds a key across dispatches would have to keep the
    // advertised delay above its longest hold.
    //
    // Like the user seat, keep the last values while no keyboard is active.
    void sync_key_repeat(const SP<IKeyboard>& keyboard) {
        if (!keyboard) return;
        const auto rate = std::max(0, keyboard->m_repeatRate), delay = std::max(0, keyboard->m_repeatDelay);
        if (rate == repeat_rate && delay == repeat_delay) return;
        repeat_rate = rate; repeat_delay = delay;
        for (auto& k : keyboards)
            if (!k->dead && k->wl->resource() && k->wl->version() >= 4) k->wl->sendRepeatInfo(rate, delay);
    }
    void sync_keymap() {
        initialize_agent_keymap();
        const auto keyboard = g_pSeatManager->m_keyboard.lock();
        sync_key_repeat(keyboard);
        if (physical_keyboard != keyboard) {
            if (physical_keymap_present) desktop_transition();
            keymap_listener.reset();
            physical_keyboard = keyboard;
            if (keyboard && kProduction)
                keymap_listener = keyboard->m_keyboardEvents.keymap.listen([this](IKeyboard::SKeymapEvent) {
                    desktop_transition();
                });
        }
        if (!keyboard || keyboard->m_xkbKeymapV1FD.get() < 0 || keyboard->m_xkbKeymapV1String.empty()) {
            if (physical_keymap_present) desktop_transition();
            physical_keymap_present = false;
            return;
        }
        physical_keymap_present = true;
        if (physical_keyboard_state && physical_keymap_text == keyboard->m_xkbKeymapV1String) return;
        // Foreground keyboard delivery uses the primary seat. Keep a separate
        // compiled state for it; the background seats retain their fixed US map.
        auto* context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        auto* map = context ? xkb_keymap_new_from_string(context, keyboard->m_xkbKeymapV1String.c_str(),
            XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS) : nullptr;
        auto* state = map ? xkb_state_new(map) : nullptr;
        if (!state) {
            if (state) xkb_state_unref(state);
            if (map) xkb_keymap_unref(map);
            if (context) xkb_context_unref(context);
            throw std::runtime_error("primary XKB state unavailable");
        }
        desktop_transition();
        if (physical_keyboard_state) xkb_state_unref(physical_keyboard_state);
        if (physical_keymap) xkb_keymap_unref(physical_keymap);
        if (physical_xkb_context) xkb_context_unref(physical_xkb_context);
        physical_keyboard_state = state; physical_keymap = map; physical_xkb_context = context;
        // Keyboard qualification: every key the KEY command can press must type the
        // same keysym as the canonical agent keymap. Checked once per keymap, so a
        // different layout refuses before activation rather than mid-string.
        typing_keymap = !kProduction || typing_keymap_equivalent(map, keymap);
        physical_keymap_text = keyboard->m_xkbKeymapV1String;
    }
    void start() {
        sync_keymap();
        timer = wl_event_loop_add_timer(g_pCompositor->m_wlEventLoop, tick, this);
        if (!timer) throw std::runtime_error("input timer registration failed");
        global = wl_global_create(g_pCompositor->m_wlDisplay, &wl_seat_interface, 9, this, bind_seat);
        if (!global) throw std::runtime_error("synthetic seat unavailable");
        wl_event_source_timer_update(timer, 16);
    }
    void resume() {
        if (retired || desktop_generation == UINT64_MAX)
            throw std::runtime_error("restart desktop before replacing input seats");
        if (!suspended) return;
        sync_keymap();
        // Every new admission period has fresh identity and no inherited grant.
        epoch = nonce();
        listener = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (listener < 0) throw std::runtime_error("input socket unavailable");
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
        // Never unlink a pre-existing socket owned by another plugin instance.
        if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
            throw std::runtime_error("input socket bind refused");
        if (!socket_path.capture(path))
            throw std::runtime_error("input socket ownership unavailable");
        socket_cleanup = "bound";
        if (chmod(path.c_str(), 0600) != 0 || listen(listener, 8) != 0)
            throw std::runtime_error("input socket setup failed");
        listen_source = wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop, listener, WL_EVENT_READABLE, accept_ready, this);
        if (!listen_source) throw std::runtime_error("input event loop registration failed");
        suspended = false;
    }
    void cleanup_socket() {
        using Result = OwnedSocketPath::CleanupResult;
        switch (socket_path.cleanup()) {
            case Result::NotCaptured: break;
            case Result::Removed: socket_cleanup = "removed"; break;
            case Result::AlreadyAbsent: socket_cleanup = "absent"; break;
            case Result::Replaced: socket_cleanup = "replacement_preserved"; break;
            case Result::Failed: socket_cleanup = "failed"; break;
        }
    }
    void suspend(std::string_view reason = "plugin_disabled") {
        suspended = true;
        revoke(reason, reason != "plugin_shutdown");
        reservation = nullptr;
        if (listen_source) wl_event_source_remove(listen_source);
        listen_source = nullptr;
        clients.clear();
        if (listener >= 0) close(listener);
        listener = -1;
        cleanup_socket();
        // Keep the globals, capabilities, and client resources stable. Removing
        // and recreating them makes existing apps lose their agent input path.
    }
    void retire() {
        // wl_global removal does not revoke existing protocol objects. Destroying
        // those objects immediately disconnects clients that still send release
        // or cursor requests (observed with foot). Retain inert resources and
        // callbacks until compositor exit; input-enabled modules are NODELETE.
        suspend("plugin_shutdown");
        keymap_listener.reset();
        retired = true;
        for (auto& seat : seats)
            if (!seat->dead && seat->wl->resource())
                seat->wl->sendCapabilities(static_cast<wl_seat_capability>(0));
        if (global) wl_global_remove(global);
    }
    ~Impl() {
        revoke("plugin_shutdown");
        if (global) wl_global_destroy(global);
        if (listen_source) wl_event_source_remove(listen_source);
        if (timer) wl_event_source_remove(timer);
        clients.clear();
        // Destroy all plugin-owned resources before unloading code callbacks.
        pointers.clear(); keyboards.clear(); touches.clear(); seats.clear();
        if (listener >= 0) close(listener);
        cleanup_socket();
        if (keyboard_state) xkb_state_unref(keyboard_state);
        if (keymap) xkb_keymap_unref(keymap);
        if (xkb_context_) xkb_context_unref(xkb_context_);
        if (physical_keyboard_state) xkb_state_unref(physical_keyboard_state);
        if (physical_keymap) xkb_keymap_unref(physical_keymap);
        if (physical_xkb_context) xkb_context_unref(physical_xkb_context);
        if (keymap_fd >= 0) close(keymap_fd);
    }
    std::uint32_t serial() const {
        const auto s = wl_display_next_serial(g_pCompositor->m_wlDisplay);
        agent_serials.note(s, s);
        return s;
    }
    bool available() const { return !retired && !suspended && session_available(); }
    static void bind_seat(wl_client* client, void* data, std::uint32_t version, std::uint32_t id) {
        auto& self = *static_cast<Impl*>(data);
        if (self.seats.size() >= kMaxResources) { wl_client_post_no_memory(client); return; }
        auto seat = std::make_unique<Seat>();
        auto* entry = seat.get();
        seat->wl = makeShared<CWlSeat>(client, std::min(version, 9u), id);
        if (!seat->wl->resource()) { wl_client_post_no_memory(client); return; }
        // Stock resource lookup casts CWlSeat::data to CWLSeatResource. Null is
        // intentional: an agent serial must not authorize primary-seat WM grabs.
        seat->wl->setData(nullptr);
        seat->wl->setRelease([entry](CWlSeat*) { entry->dead = true; });
        seat->wl->setOnDestroy([entry](CWlSeat*) { entry->dead = true; });
        seat->wl->setGetPointer([&self](CWlSeat* r, std::uint32_t child) { self.add_pointer(r, child); });
        seat->wl->setGetKeyboard([&self](CWlSeat* r, std::uint32_t child) { self.add_keyboard(r, child); });
        seat->wl->setGetTouch([&self](CWlSeat* r, std::uint32_t child) { self.add_touch(r, child); });
        if (version >= 2) seat->wl->sendName(kProduction ?
            (self.lane == 0 ? "Cua-Agent" : "Cua-Agent-2") :
            (self.lane == 0 ? "Cua-Test-Agent" : "Cua-Test-Agent-2"));
        seat->wl->sendCapabilities(static_cast<wl_seat_capability>(self.retired ? 0 :
            WL_SEAT_CAPABILITY_POINTER | (self.keyboard_state ? WL_SEAT_CAPABILITY_KEYBOARD : 0)));
        self.seats.push_back(std::move(seat));
    }
    void add_pointer(CWlSeat* seat, std::uint32_t id) {
        if (pointers.size() >= kMaxResources) { seat->noMemory(); return; }
        auto p = std::make_unique<Pointer>(); auto* entry = p.get();
        p->wl = makeShared<CWlPointer>(seat->client(), seat->version(), id);
        if (!p->wl->resource()) { seat->noMemory(); return; }
        p->wl->setData(nullptr);
        p->wl->setRelease([entry](CWlPointer*) { entry->dead = true; });
        p->wl->setOnDestroy([entry](CWlPointer*) { entry->dead = true; });
        p->wl->setSetCursor([](CWlPointer*, std::uint32_t, wl_resource*, std::int32_t, std::int32_t) {});
        pointers.push_back(std::move(p));
    }
    void add_keyboard(CWlSeat* seat, std::uint32_t id) {
        if (!keyboard_state || keyboards.size() >= kMaxResources) { seat->noMemory(); return; }
        auto k = std::make_unique<Keyboard>(); auto* entry = k.get();
        k->wl = makeShared<CWlKeyboard>(seat->client(), seat->version(), id);
        if (!k->wl->resource()) { seat->noMemory(); return; }
        k->wl->setData(nullptr);
        k->wl->setRelease([entry](CWlKeyboard*) { entry->dead = true; });
        k->wl->setOnDestroy([entry](CWlKeyboard*) { entry->dead = true; });
        k->wl->sendKeymap(WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, keymap_fd, keymap_text.size() + 1);
        if (seat->version() >= 4) k->wl->sendRepeatInfo(repeat_rate, repeat_delay);
        keyboards.push_back(std::move(k));
    }
    void add_touch(CWlSeat* seat, std::uint32_t id) {
        // Never advertised. A valid inert resource is safer than a dangling id.
        if (touches.size() >= kMaxResources) { seat->noMemory(); return; }
        auto t = std::make_unique<Touch>(); auto* entry = t.get();
        t->wl = makeShared<CWlTouch>(seat->client(), seat->version(), id);
        if (!t->wl->resource()) { seat->noMemory(); return; }
        t->wl->setData(nullptr);
        t->wl->setRelease([entry](CWlTouch*) { entry->dead = true; });
        t->wl->setOnDestroy([entry](CWlTouch*) { entry->dead = true; });
        touches.push_back(std::move(t));
    }
    static std::optional<std::array<double, 6>> target_geometry(
        const PHLWINDOW& window, const SP<CWLSurfaceResource>& surface, bool xwayland = false) {
        if (!window || !surface || !window->m_isMapped || window->isHidden() ||
            window->m_isX11 != xwayland || window->resource() != surface || !surface->m_mapped || !surface->good()) return std::nullopt;
        // Match Driver's captured client surface, not the decorated window box
        // (which includes compositor borders and shifts clicks by their width).
        const auto box = window->surfaceLogicalBox(); const auto surface_box = box;
        if (!box || !surface_box || box->w <= 0 || box->h <= 0 || box->w > 32767 || box->h > 32767) return std::nullopt;
        std::array<double, 6> geometry{box->x, box->y, box->w, box->h, surface_box->x, surface_box->y};
        if (!std::ranges::all_of(geometry, [](double value) { return std::isfinite(value); })) return std::nullopt;
        return geometry;
    }
    bool refresh(Client& c) {
        const auto geometry = target_geometry(c.window.lock(), c.surface.lock(), c.xwayland);
        if (!geometry) return false;
        if (*geometry != c.geometry) {
            if (c.revision == UINT64_MAX) return false;
            c.geometry = *geometry; ++c.revision;
        }
        return true;
    }
    bool primary_conflict(const Client& c) const {
        if (c.route == InputRoute::primary_foreground)
            return foreground_started && !foreground_activating && !foreground_guard(c).can_dispatch(foreground_needs_pointer);
        // The agent seat has its own pointer, so its pointer actions may share
        // a native app with the human, as on macOS. Its keys may not; see
        // keyboard_conflict. An X app has one client pointer for all its
        // windows, and its reactions to the agent would reach the human's
        // keyboard, so X targets and the routes that borrow the human's own
        // resources still stay out of the human's app.
        if (c.route == InputRoute::independent && !c.xwayland) return false;
        return primary_conflict(c.surface.lock());
    }
    std::string_view keyboard_conflict(const Client& c, std::uint64_t capability) const {
        return agent_keyboard_refusal(c.route, c.xwayland, capability,
            g_pSeatManager->m_state.keyboardFocus.lock(), c.surface.lock());
    }
    // XWayland is a single Wayland client for every X app. Each input user has
    // its own X master pair there, so only the same X process conflicts.
    static bool same_app(const SP<CWLSurfaceResource>& a, const SP<CWLSurfaceResource>& b) {
        if (!a || !b || a->client() != b->client()) return false;
        PHLWINDOW window_a, window_b;
        for (const auto& w : Desktop::windowState()->windows()) {
            if (w->resource() == a) window_a = w;
            if (w->resource() == b) window_b = w;
        }
        return !(window_a && window_b && window_a->m_isX11 && window_b->m_isX11 &&
            window_a->getPID() > 0 && window_a->getPID() != window_b->getPID());
    }
    bool primary_conflict(const SP<CWLSurfaceResource>& surface) const {
        if (!surface) return true;
        // Conservative per-client refusal avoids toolkit-global cross-window state.
        return same_app(g_pSeatManager->m_state.pointerFocus.lock(), surface) ||
            same_app(g_pSeatManager->m_state.keyboardFocus.lock(), surface);
    }
    bool agent_conflict(const Client& c) const {
        if (c.route == InputRoute::primary_foreground)
            for (const auto* peer : peers)
                if (peer && peer != this && peer->lease && peer->lease->route == InputRoute::primary_foreground) return true;
        return agent_conflict(c.surface.lock());
    }
    bool agent_conflict(const SP<CWLSurfaceResource>& surface) const {
        if (!surface) return true;
        for (const auto* peer : peers) {
            if (!peer || peer == this) continue;
            // Passive hover still participates after its transport owner dies.
            if (peer->pointer_target.entered() && same_app(peer->pointer_target.surface(), surface)) return true;
            if (peer->lease && same_app(peer->lease->surface.lock(), surface)) return true;
        }
        return false;
    }
    void retire_orphan_hover(const Client& c) {
        for (auto* peer : peers) {
            if (!peer || peer == this) continue;
            const InputLaneActivity activity{
                .reserved = peer->reservation != nullptr,
                .leased = peer->lease != nullptr,
                .dragging = peer->drag.has_value(),
                .button = peer->held_button != 0,
                .keys = !peer->held_keys.empty(),
                .keyboard_focus = std::ranges::any_of(peer->keyboards, [](const auto& k) { return bool(k->focus); }),
                .capabilities = peer->capabilities != 0,
                .grant = peer->grant.deadline() != Clock::time_point{},
                .expiry = peer->expires != Clock::time_point{},
                .reservation_without_target = peer->reservation && !peer->reservation->has_bound_target,
            };
            // Driver claims the first free lane before TARGET. Retire only a
            // matching, completely inert peer hover so opposite-order reuse
            // cannot strand a target on another lane. A fresh claimant that
            // has never bound a target does not adopt inherited hover; an
            // existing target owner remains protected even after STOP.
            if (peer->pointer_target.reclaimable_for(c.surface.lock(), activity)) peer->leave_pointer();
        }
    }
    void invalidate(Client& c, bool clear_pointer = true) {
        if (lease == &c || (clear_pointer && c.surface.lock() && pointer_target.surface() == c.surface.lock()))
            revoke("stale_target", !clear_pointer);
        c.token.clear(); c.window.reset(); c.surface.reset();
        // Replay high-water belongs to the old target binding. A genuinely
        // new token may receive a shorter grant after Stop or target change.
        c.approved_deadline = 0;
    }
    void release_pointer_button() {
        if (!held_button) return;
        for (auto& p : pointers) {
            const auto surface = p->focus.lock();
            if (!p->dead && p->wl->resource() && surface && surface->good()) {
                p->wl->sendButton(serial(), event_ms(), held_button, WL_POINTER_BUTTON_STATE_RELEASED);
                // A complete release frame is delivery, not client processing proof.
                if (p->wl->version() >= 5) p->wl->sendFrame();
            }
        }
        held_button = 0;
    }
    void leave_pointer() {
        release_pointer_button();
        for (auto& p : pointers) {
            const auto surface = p->focus.lock();
            if (!p->dead && p->wl->resource() && surface && surface->good()) {
                p->wl->sendLeave(serial(), surface->getResource().get());
                if (p->wl->version() >= 5) p->wl->sendFrame();
            }
            p->focus.reset();
        }
        pointer_target.reset();
        pointer_unmap.reset(); pointer_destroy.reset();
    }
    void leave_keyboard() {
        for (auto& k : keyboards) {
            const auto surface = k->focus.lock();
            if (!k->dead && k->wl->resource() && surface && surface->good()) {
                for (auto key : held_keys) k->wl->sendKey(serial(), event_ms(), key, WL_KEYBOARD_KEY_STATE_RELEASED);
                k->wl->sendModifiers(serial(), 0, 0, 0, 0);
                k->wl->sendLeave(serial(), surface->getResource().get());
            }
            k->focus.reset();
        }
        held_keys.clear();
        if (keyboard_state) xkb_state_unref(keyboard_state);
        keyboard_state = keymap ? xkb_state_new(keymap) : nullptr;
        if (physical_keyboard_state) xkb_state_unref(physical_keyboard_state);
        physical_keyboard_state = physical_keymap ? xkb_state_new(physical_keymap) : nullptr;
    }
    void retire_grant() {
        lease = nullptr; capabilities = 0; grant.reset(); expires = {};
    }
    void complete_action() {
        // All buttons/keys were released by the operation. Keyboard focus is
        // action-scoped; passive pointer focus belongs only to its live target.
        // Never send a gratuitous leave immediately after a drag's release:
        // clients may requeue their release behind that leave while coalescing
        // motion. This is not a client-processing acknowledgement.
        finish_foreground();
        leave_keyboard();
        retire_grant();
    }
    // Hyprland honours xdg_popup.grab and wl_data_device.start_drag without
    // checking the seat or serial, then moves the human's focus to the popup
    // or ties the drop to the human's next button release. Refuse either for a
    // client this lane is driving or has just driven, unless the human is
    // already interacting with that client.
    // Background dispatch only. A foreground action already moved human focus
    // to its target, so that client's popups and drags are the human's own.
    void note_dispatch(const Client& c) {
        recent_target = c.surface;
        recent_dispatch = Clock::now();
        if (const auto w = c.window.lock(); w && w->m_workspace)
            recent_app = {w->getPID(), w->m_isX11, w->m_workspace->m_id, w->m_workspace->m_name, w->monitorID()};
    }
    bool owns_reaction(wl_client* client) const {
        if (!client) return false;
        const auto pointer = g_pSeatManager->m_state.pointerFocus.lock();
        const auto keyboard = g_pSeatManager->m_state.keyboardFocus.lock();
        if ((pointer && pointer->client() == client) || (keyboard && keyboard->client() == client)) return false;
        if (lease && lease->route != InputRoute::primary_foreground)
            if (const auto s = lease->surface.lock(); s && s->client() == client) return true;
        if (drag && drag->client && drag->client->route != InputRoute::primary_foreground)
            if (const auto s = drag->client->surface.lock(); s && s->client() == client) return true;
        const auto recent = recent_target.lock();
        return recent && recent->client() == client && Clock::now() - recent_dispatch < kReactionWindow;
    }
    static pid_t pid_of(const SP<CWLSurfaceResource>& surface) {
        pid_t pid = 0;
        if (surface && surface->client()) wl_client_get_credentials(surface->client(), &pid, nullptr, nullptr);
        return pid;
    }
    // The window this lane drives in the app of a new window. When the human
    // is in that app too, only if the agent gave it a command after the human
    // last did. Apps are processes: X apps share one client, and Electron
    // shows its GTK dialogs over a second Wayland connection.
    SP<CWLSurfaceResource> app_target(const SP<CWLSurfaceResource>& surface, bool x, const std::vector<HumanCommand>& commands) const {
        const auto same = [&](const SP<CWLSurfaceResource>& other) {
            return x ? same_app(other, surface) : pid_of(surface) > 0 && pid_of(other) == pid_of(surface);
        };
        SP<CWLSurfaceResource> target;
        Clock::time_point agent;
        if (const auto s = lease && lease->route != InputRoute::primary_foreground ? lease->surface.lock() : nullptr; same(s)) {
            target = s; agent = Clock::now();
        } else if (const auto s = recent_target.lock(); Clock::now() - recent_dispatch < kReactionWindow && same(s)) {
            target = s; agent = recent_dispatch;
        }
        if (!target || (!same(g_pSeatManager->m_state.pointerFocus.lock()) && !same(g_pSeatManager->m_state.keyboardFocus.lock()))) return target;
        std::optional<Clock::time_point> human;
        for (const auto& c : commands)
            if (same(c.surface.lock()) && (!human || c.at > *human)) human = c.at;
        return agent_commanded_last(agent, human) ? target : nullptr;
    }
    // The app `window` replaces the window this lane drove in, if it does.
    const DrivenApp* replaced_app(const PHLWINDOW& window, const std::vector<HumanCommand>& commands) const {
        const auto same = [&](const PHLWINDOW& w) { return w && recent_app.pid > 0 && w->m_isX11 == recent_app.x && w->getPID() == recent_app.pid; };
        const auto window_of = [](const SP<CWLSurfaceResource>& s) {
            const auto wl = s ? Desktop::View::CWLSurface::fromResource(s) : nullptr;
            return wl ? Desktop::View::CWindow::fromView(wl->view()) : nullptr;
        };
        std::optional<Clock::time_point> human;
        for (const auto& c : commands)
            if (same(window_of(c.surface.lock())) && (!human || c.at > *human)) human = c.at;
        const Replacement replacement{
            .same_app = !window->parent() && same(window),
            .driven_gone = !recent_target.lock(),
            .recent = Clock::now() - recent_dispatch < kReactionWindow,
            .human_in_app = same(Desktop::focusState()->window()),
            .agent_last = agent_commanded_last(recent_dispatch, human),
        };
        return agent_replacement(replacement) ? &recent_app : nullptr;
    }
    // The window this lane is driving, or drove within the reaction window.
    bool drives(const SP<CWLSurfaceResource>& surface) const {
        if (!surface) return false;
        if (lease && lease->route != InputRoute::primary_foreground && lease->surface.lock() == surface) return true;
        return recent_target.lock() == surface && Clock::now() - recent_dispatch < kReactionWindow;
    }
    // When this lane last delivered to the window whose own surface is `surface`.
    std::optional<Clock::time_point> last_dispatch_to(const SP<CWLSurfaceResource>& surface) const {
        if (!surface) return std::nullopt;
        if (lease && lease->route != InputRoute::primary_foreground && lease->surface.lock() == surface) return Clock::now();
        if (recent_target.lock() == surface) return recent_dispatch;
        return std::nullopt;
    }
    void revoke(std::string_view reason, bool retain_pointer = false, ForegroundFailure failure = {ForegroundFailureReason::none}) {
        if (lease && trace && reason != "completed") trace->mark("agent_cancel", lane + 1);
        if (drag && drag->client && !drag->client->dead)
            send(*drag->client, failure.reason == ForegroundFailureReason::none ?
                refusal(foreground_started ? "foreground_partial_unknown" : reason) :
                refusal(foreground_started ? "foreground_partial_unknown" : reason, failure));
        drag.reset();
        finish_foreground();
        return_primary();
        if (retain_pointer) release_pointer_button(); else leave_pointer();
        leave_keyboard(); retire_grant();
    }
    void cancel_authority(std::string_view reason, bool retain_pointer = true) {
        revoke(reason, retain_pointer);
        // Invalidate pending authority as well as active work. This also makes
        // unused signed renewals unusable in experimental builds. Keep the lane
        // reservation; v3 requires fresh TARGET admission for the next action.
        for (auto& c : clients) invalidate(*c, false);
    }
    void desktop_transition() {
        // Signals can fire before Hyprland updates its aggregate state. Revoke
        // unconditionally; an off/on pair between timer ticks must not revive
        // authority. Kill pending connections as well as the active lease so
        // a pre-transition signed grant cannot be approved after unlock.
        // Admission is gone immediately, but an unchanged live target may keep
        // inert hover just as after normal completion. A leave directly after
        // the release can interfere with the client's drag-release processing.
        revoke("desktop_changed", true);
        for (auto& c : clients) {
            // Observers and operator-control connections own no action target.
            // Keep them alive so Stop/status/evidence survives a transition.
            if (reservation != c.get() && c->token.empty()) continue;
            c->dead = true;
            if (c->source) wl_event_source_remove(c->source);
            c->source = nullptr;
            if (c->fd >= 0) close(c->fd);
            c->fd = -1;
        }
        reservation = nullptr;
        if (desktop_generation == UINT64_MAX)
            suspended = true;
        else
            ++desktop_generation;
    }
    void send(Client& c, const std::string& packet) {
        if (c.dead) return;
        const auto n = ::send(c.fd, packet.data(), packet.size(), MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n < 0 || static_cast<std::size_t>(n) != packet.size()) c.dead = true;
    }
    void reap_clients() {
        if (lease && lease->dead) revoke("disconnected", true);
        if (reservation && reservation->dead) reservation = nullptr;
        std::erase_if(clients, [](auto& c) { return c->dead; });
    }
    static int accept_ready(int, std::uint32_t, void* data) {
        auto& self = *static_cast<Impl*>(data);
        // Closed connections stay in clients until reaped. Without this, a
        // burst of reconnects within one timer tick hits the limit.
        self.reap_clients();
        for (unsigned i = 0; i < 8; ++i) {
            const auto fd = accept4(self.listener, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (fd < 0) break;
            ucred credentials{}; socklen_t size = sizeof(credentials);
            if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &size) != 0 ||
                size != sizeof(credentials) || credentials.uid != getuid() || self.clients.size() >= kMaxClients) { close(fd); continue; }
            bool owned = false;
            try {
                auto c = std::make_unique<Client>(); c->owner = &self; c->fd = fd; owned = true; c->challenge = nonce();
                c->source = wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop, fd, WL_EVENT_READABLE, client_ready, c.get());
                if (!c->source) continue;
                self.clients.push_back(std::move(c));
            } catch (...) { if (!owned) close(fd); }
        }
        return 0;
    }
    static int client_ready(int fd, std::uint32_t mask, void* data) {
        auto& c = *static_cast<Client*>(data); auto& self = *c.owner;
        if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) c.dead = true;
        for (unsigned i = 0; i < 8 && !c.dead; ++i) {
            // Check before reading or changing HELLO state, even if the event
            // loop's periodic expiry callback has not run yet.
            if (c.deadline.expired(c.hello, Clock::now())) { c.dead = true; break; }
            std::array<char, kMaxPacket> buffer{};
            const auto n = recv(fd, buffer.data(), buffer.size(), MSG_DONTWAIT | MSG_TRUNC);
            if (n < 0) { if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) c.dead = true; break; }
            if (!n) { c.dead = true; break; }
            const auto received = Clock::now();
            if (c.deadline.expired(c.hello, received)) { c.dead = true; break; }
            c.deadline.touch(received);
            if (static_cast<std::size_t>(n) > buffer.size()) { self.send(c, refusal("invalid_request")); continue; }
            try { self.request(c, fields(std::string_view(buffer.data(), n))); }
            catch (const ForegroundFailure& failure) {
                const bool started = c.foreground_attempted;
                const bool drag_reply = self.drag && self.drag->client == &c;
                self.revoke("foreground_partial_unknown", false, failure);
                if (!drag_reply) self.send(c, refusal(ForegroundFailure::code(started), failure));
            }
            catch (...) {
                const bool foreground_partial = c.foreground_attempted;
                const bool foreground_drag_reply = foreground_partial && self.drag && self.drag->client == &c;
                if (kProduction && (self.lease == &c || self.reservation == &c)) self.revoke("invalid_request");
                if (!foreground_drag_reply) self.send(c, refusal(foreground_partial ? "foreground_partial_unknown" : "invalid_request"));
            }
        }
        if (c.dead) {
            if (self.lease == &c) self.revoke("disconnected", true);
            if (self.reservation == &c) self.reservation = nullptr;
        }
        return 0;
    }
#ifdef CUA_HYPRLAND_TEST_INPUT
    bool valid_signature(const std::string& message, const std::string& signature) const {
        if (signature.size() != 128) return false;
        const auto bytes = unhex(signature);
        auto* key = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, public_key.data(), public_key.size());
        auto* ctx = EVP_MD_CTX_new();
        const bool ok = key && ctx && EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, key) == 1 &&
            EVP_DigestVerify(ctx, bytes.data(), bytes.size(), reinterpret_cast<const unsigned char*>(message.data()), message.size()) == 1;
        EVP_MD_CTX_free(ctx); EVP_PKEY_free(key); return ok;
    }
    void approve(Client& c, const std::vector<std::string>& f) {
        if (f.size() != 6) { send(c, refusal("invalid_grant")); return; }
        const auto deadline = number(f[3]); const auto caps = number(f[4]); const auto now = unix_ms();
        if (deadline <= now || deadline - now > 60000 || caps == 0 || caps > 15) { send(c, refusal("invalid_grant")); return; }
        const auto message = std::format("CUA_TEST_LEASE_1\n{}\n{}\n{}\n{}\n{}\n", epoch, f[1], f[2], deadline, caps);
        if (!valid_signature(message, f[5])) { send(c, refusal("invalid_grant")); return; }
        const auto found = std::ranges::find_if(clients, [&](auto& peer) {
            return !peer->dead && peer->hello && peer->challenge == f[1] && peer->token == f[2] && !peer->token.empty();
        });
        if (found == clients.end() || !refresh(**found)) { send(c, refusal("stale_target")); return; }
        if (reservation != found->get()) { send(c, refusal("lane_not_claimed")); return; }
        if (lease) { send(c, refusal("lease_busy")); return; }
        if (agent_conflict(**found)) { send(c, refusal("agent_target_busy")); return; }
        if (deadline <= (*found)->approved_deadline) { send(c, refusal("invalid_grant")); return; }
        if (!available()) { send(c, refusal("session_unavailable")); return; }
        // Bind steady-clock lifetime once; a clock change cannot extend a lease.
        lease = found->get(); capabilities = caps; expires = Clock::now() + std::chrono::milliseconds(deadline - now);
        lease->approved_deadline = deadline;
        // Approval begins the bounded active period. Time spent waiting for
        // the external operator must not consume the input connection's idle
        // budget while its newly approved lease is still valid.
        lease->deadline.touch(Clock::now());
        if (trace) trace->mark("agent_approved", lane + 1);
        send(c, R"({"ok":true})");
    }
#endif
    bool point(Client& c, double x, double y) const {
        return std::isfinite(x) && std::isfinite(y) && x >= 0 && y >= 0 && x < c.geometry[2] && y < c.geometry[3];
    }
    using PointerSurfaceAt = PointerSurface<SP<CWLSurfaceResource>, Vector2D>;
    // Hyprland places popups from the window's current position, which is
    // also the corner of c.geometry, so window-local x, y map onto them.
    PointerSurfaceAt pointer_hit_at(const Client& c, double x, double y) const {
        using Hit = std::pair<SP<CWLSurfaceResource>, Vector2D>;
        const auto root = c.surface.lock();
        const auto window = c.window.lock();
        if (!root || !window) return {};
        Hit popup;
        const Vector2D global{c.geometry[0] + x, c.geometry[1] + y};
        if (!window->m_isX11 && window->m_popupHead)
            if (const auto found = window->m_popupHead->at(global, true); found && found->wlSurface() && found->wlSurface()->resource())
                popup = {found->wlSurface()->resource(), global - found->coordsGlobal()};
        const Vector2D local{x + c.geometry[0] - c.geometry[4], y + c.geometry[1] - c.geometry[5]};
        return classify_pointer_hit(root, popup, root->at(local, true));
    }
    // Where a pointer action at x, y lands, or nothing after sending its refusal.
    std::optional<PointerSurfaceAt> pointer_target_at(Client& c, double x, double y, bool drag) {
        const auto target = pointer_hit_at(c, x, y);
        const auto grab = g_pSeatManager->m_seatGrab;
        const bool human_grab = target.hit == PointerHit::popup && grab && grab->accepts(target.surface);
        if (const auto refused = pointer_refusal(point(c, x, y), target.hit, drag, human_grab); !refused.empty()) {
            send(c, refusal(refused));
            return std::nullopt;
        }
        return target;
    }
    bool unique_primary_seat(wl_client* client) const {
        struct Scan {
            const Impl* self;
            ForegroundSeatBindings bindings;
        } scan{this, {}};
        // Hyprland's public lookup returns only the first primary binding.
        // Count classes without casting arbitrary protocol user data. Agent
        // seats are excluded only by exact resource identity, never by name.
        wl_client_for_each_resource(client, [](wl_resource* resource, void* data) {
            auto& scan = *static_cast<Scan*>(data);
            const std::string_view resource_class = wl_resource_get_class(resource);
            if (resource_class != "wl_seat") return WL_ITERATOR_CONTINUE;
            bool owned = false;
            for (const auto* peer : scan.self->peers) {
                if (!peer) continue;
                for (const auto& seat : peer->seats)
                    if (seat->wl->resource() == resource) { owned = true; break; }
                if (owned) break;
            }
            scan.bindings.observe(resource_class, owned);
            return WL_ITERATOR_CONTINUE;
        }, &scan);
        if (!scan.bindings.unique()) return false;
        if (!foreground_started) return true;
        // A release/rebind or new child resource during a drag must not leave
        // our captured delivery set different from the primary seat's set.
        const auto seat = g_pSeatManager->seatResourceForClient(client);
        if (!seat || seat != foreground_seat.lock()) return false;
        return foreground_resources_match(seat->m_pointers, foreground_pointers) &&
            foreground_resources_match(seat->m_keyboards, foreground_keyboards);
    }
    PrimaryBackgroundGuard primary_background_guard(const Client& c) const {
        const auto root = c.surface.lock();
        return {
            .primary_binding = root && unique_primary_seat(root->client()),
            .human_on_client = primary_conflict(root),
            .peer_conflict = agent_conflict(c),
            // A grab or drag routes the human's pointer through the compositor
            // regardless of focus; borrowing resources under it would race.
            .grab = bool(g_pSeatManager->m_seatGrab) || bool(g_layoutManager->dragController()->target()),
            .dnd = PROTO::data && PROTO::data->dndActive(),
            .constraint = g_pInputManager->isConstrained(),
        };
    }
    void borrow_primary(Client& c, bool needs_pointer, bool needs_keyboard) {
        const auto root = c.surface.lock();
        const auto seat = root ? g_pSeatManager->seatResourceForClient(root->client()) : nullptr;
        borrowed_pointers.clear(); borrowed_keyboards.clear();
        if (!seat || !seat->good()) return;
        if (needs_pointer)
            for (const auto& p : seat->m_pointers) if (p && p->good()) borrowed_pointers.push_back(p);
        if (needs_keyboard)
            for (const auto& k : seat->m_keyboards) if (k && k->good()) borrowed_keyboards.push_back(k);
        borrowed_surface = root;
        borrowed_seat = seat;
    }
    // Every borrowed resource still belongs to a client that has no human
    // focus, so CSeatManager will not send it anything until we finish.
    bool borrowed_live(Client& c) const {
        const auto root = borrowed_surface.lock();
        return root && root == c.surface.lock() && root->good() && primary_background_guard(c).failure().empty();
    }
    void borrowed_pointer_enter(const SP<CWLSurfaceResource>& surface, const Vector2D& local) {
        for (const auto& weak : borrowed_pointers)
            if (const auto p = weak.lock(); p && p->good()) { p->sendEnter(surface, local); p->sendMotion(event_ms(), local); p->sendFrame(); }
    }
    void borrowed_button(std::uint32_t code, bool pressed) {
        if (pressed) held_button = code;
        for (const auto& weak : borrowed_pointers)
            if (const auto p = weak.lock(); p && p->good()) {
                p->sendButton(event_ms(), code, pressed ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED);
                p->sendFrame();
            }
        if (pressed) forget_press(borrowed_seat, borrowed_surface, code);
        if (!pressed) held_button = 0;
    }
    // CWLPointerResource::sendButton records a press as a serial the client
    // may start a compositor move or resize with, and that move would follow
    // the human's pointer until their next release. An agent press on a
    // primary seat must never do that, so drop it at once. Agent seats need
    // nothing: Hyprland ignores a move or resize naming a seat it did not make.
    static void forget_press(const WP<CWLSeatResource>& seat, const WP<CWLSurfaceResource>& surface, std::uint32_t code) {
        if (const auto s = seat.lock(); s && s->good()) g_pSeatManager->clearPointerButtonSerials(s, surface.lock(), code);
    }
    void borrowed_key(std::uint32_t code, bool pressed) {
        xkb_state_update_key(physical_keyboard_state, code + 8, pressed ? XKB_KEY_DOWN : XKB_KEY_UP);
        if (pressed) held_keys.push_back(code); else std::erase(held_keys, code);
        for (const auto& weak : borrowed_keyboards)
            if (const auto k = weak.lock(); k && k->good()) {
                k->sendKey(event_ms(), code, pressed ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED);
                k->sendMods(xkb_state_serialize_mods(physical_keyboard_state, XKB_STATE_MODS_DEPRESSED),
                    xkb_state_serialize_mods(physical_keyboard_state, XKB_STATE_MODS_LATCHED),
                    xkb_state_serialize_mods(physical_keyboard_state, XKB_STATE_MODS_LOCKED),
                    xkb_state_serialize_layout(physical_keyboard_state, XKB_STATE_LAYOUT_EFFECTIVE));
            }
    }
    // Leave exactly what we entered. Once human focus reaches this client,
    // CSeatManager has already left these resources and re-entered the human's
    // surface on them; they are no longer ours to release or leave.
    void return_primary() {
        const bool borrowed = !borrowed_pointers.empty() || !borrowed_keyboards.empty();
        const auto root = borrowed_surface.lock();
        if (root && root->good()) {
            const auto pointer = g_pSeatManager->m_state.pointerFocus.lock();
            const auto keyboard = g_pSeatManager->m_state.keyboardFocus.lock();
            if (!pointer || pointer->client() != root->client())
                for (const auto& weak : borrowed_pointers)
                    if (const auto p = weak.lock(); p && p->good()) {
                        if (held_button) p->sendButton(event_ms(), held_button, WL_POINTER_BUTTON_STATE_RELEASED);
                        p->sendLeave(); p->sendFrame();
                    }
            if (!keyboard || keyboard->client() != root->client())
                for (const auto& weak : borrowed_keyboards)
                    if (const auto k = weak.lock(); k && k->good()) {
                        for (auto code : held_keys) k->sendKey(event_ms(), code, WL_KEYBOARD_KEY_STATE_RELEASED);
                        k->sendMods(0, 0, 0, 0);
                        k->sendLeave();
                    }
        }
        borrowed_pointers.clear(); borrowed_keyboards.clear(); borrowed_surface.reset(); borrowed_seat.reset();
        // With nothing borrowed, a held button or key is on the agent seat, and
        // revoke() still has to release it there.
        if (borrowed) { held_button = 0; held_keys.clear(); }
        if (physical_keyboard_state) xkb_state_unref(physical_keyboard_state);
        physical_keyboard_state = physical_keymap ? xkb_state_new(physical_keymap) : nullptr;
    }
    ForegroundGuard foreground_guard(const Client& c) const {
        const auto root = c.surface.lock();
        const auto geometry = target_geometry(c.window.lock(), root);
        bool pressed = !g_pInputManager->getKeysFromAllKBs().empty();
        for (const auto& kb : g_pInputManager->m_keyboards) {
            if (!kb->m_enabled || kb->isVirtual()) continue;
            for (std::uint32_t code = 0; code <= KEY_MAX && !pressed; ++code) pressed = kb->getPressed(code);
        }
        const bool exact_root = geometry && *geometry == c.geometry;
        return {
            .exact_root = exact_root,
            .primary_binding = !exact_root || unique_primary_seat(root->client()),
            .peer_conflict = agent_conflict(c),
            .physical_keys = pressed,
            .physical_buttons = g_pInputManager->hasHeldButtons(),
            .grab = bool(g_pSeatManager->m_seatGrab) || bool(g_layoutManager->dragController()->target()) ||
                !g_pInputManager->m_exclusiveLSes.empty(),
            .dnd = PROTO::data && PROTO::data->dndActive(),
            .constraint = g_pInputManager->isConstrained(),
            .exact_keyboard_focus = root && g_pSeatManager->m_state.keyboardFocus == root &&
                Desktop::focusState()->window() == c.window.lock() && Desktop::focusState()->surface() == root,
            .exact_pointer_focus = root && g_pSeatManager->m_state.pointerFocus == root,
        };
    }
    // Hyprland reports each keyboard's raw masks in that keyboard's own keymap
    // encoding and merges shared keyboards into the primary state. Refuse a
    // keyboard action under any state other than neutral or an unambiguous
    // Num Lock, and name Caps Lock explicitly.
    std::array<std::uint32_t, 4> capture_foreground_modifiers(bool needs_keyboard) const {
        const auto physical = g_pSeatManager->m_keyboard.lock();
        if (!physical) throw ForegroundFailure{ForegroundFailureReason::physical_keyboard};
        std::array<std::uint32_t, 4> result{};
        const auto observe = [&](const auto& kb, bool primary) {
            const std::array<std::uint32_t, 4> state{kb->m_modifiersState.depressed, kb->m_modifiersState.latched,
                kb->m_modifiersState.locked, kb->m_modifiersState.group};
            if (needs_keyboard) {
                const auto failure = foreground_key_modifier_failure(state, foreground_numlock_mask(kb->m_xkbKeymap),
                    foreground_caps_mask(kb->m_xkbKeymap));
                if (failure != ForegroundFailureReason::none) throw ForegroundFailure{failure};
                // A lock from a different encoding must not be reinterpreted
                // as the primary keymap's Num Lock.
                if (state[2] && state[2] != foreground_numlock_mask(physical_keymap))
                    throw ForegroundFailure{ForegroundFailureReason::keyboard_locked};
            }
            for (unsigned i = 0; i < 3; ++i) result[i] |= state[i];
            if (primary) result[3] = state[3];
        };
        observe(physical, true);
        for (const auto& kb : g_pInputManager->m_keyboards) {
            if (kb == physical || !kb->m_enabled || !kb->shareStates() ||
                (kb->isVirtual() && g_pInputManager->shouldIgnoreVirtualKeyboard(kb))) continue;
            observe(kb, false);
        }
        return result;
    }
    void require_foreground(Client& c) {
        if (lease != &c) throw ForegroundFailure{ForegroundFailureReason::lease};
        if (c.dead) throw ForegroundFailure{ForegroundFailureReason::client_dead};
        if (!available()) throw ForegroundFailure{ForegroundFailureReason::session_unavailable};
        if (!input_layout_qualified(c.route, foreground_needs_keyboard, layout_qualified()))
            throw ForegroundFailure{ForegroundFailureReason::unsupported_layout};
        if (Clock::now() >= expires) throw ForegroundFailure{ForegroundFailureReason::lease_expired};
        // The chord was admitted under the human keyboard's state. A lock or
        // modifier change during the action, including one caused by
        // activation, changes what the remaining events mean.
        if (foreground_needs_keyboard && capture_foreground_modifiers(true) != foreground_modifiers)
            throw ForegroundFailure{ForegroundFailureReason::keyboard_state};
        const auto failure = foreground_guard(c).dispatch_failure(foreground_needs_pointer);
        if (failure != ForegroundFailureReason::none) throw ForegroundFailure{failure};
    }
    void finish_foreground() {
        foreground_activating = false;
        if (!foreground_started) return;
        // Release only our own synthetic state, and only while these resources
        // still address the original root. A focus loss must not send to its successor.
        const auto root = foreground_surface.lock();
        if (root && root->good() && g_pSeatManager->m_state.pointerFocus == root)
            for (const auto& weak : foreground_pointers)
                if (const auto p = weak.lock(); p && p->good() && held_button) {
                    p->sendButton(event_ms(), held_button, WL_POINTER_BUTTON_STATE_RELEASED);
                    p->sendFrame();
                }
        if (foreground_keyboard_used && root && root->good() && g_pSeatManager->m_state.keyboardFocus == root) {
            // Synthetic keys change only private state. If human input cancelled
            // the action, restore the human keyboard's current state, not the
            // state captured at admission.
            const auto restore = g_pSeatManager->m_keyboard.lock() ?
                capture_foreground_modifiers(false) : foreground_modifiers;
            for (const auto& weak : foreground_keyboards)
                if (const auto k = weak.lock(); k && k->good()) {
                    for (auto code : held_keys) k->sendKey(event_ms(), code, WL_KEYBOARD_KEY_STATE_RELEASED);
                    k->sendMods(restore[0], restore[1], restore[2], restore[3]);
                }
        }
        held_button = 0; held_keys.clear();
        foreground_pointers.clear(); foreground_keyboards.clear(); foreground_surface.reset(); foreground_seat.reset();
        foreground_started = false;
        foreground_keyboard_used = false;
        foreground_needs_keyboard = false;
    }
    void start_foreground(Client& c, double x, double y, bool needs_pointer, bool needs_keyboard,
                          const std::array<std::uint32_t, 4>& modifiers) {
        const auto root = c.surface.lock();
        const auto physical = g_pSeatManager->m_keyboard.lock();
        const auto failure = foreground_guard(c).activation_failure();
        if (failure != ForegroundFailureReason::none) throw ForegroundFailure{failure};
        if (needs_keyboard && !physical) throw ForegroundFailure{ForegroundFailureReason::physical_keyboard};
        if (needs_keyboard && !physical_keyboard_state) throw ForegroundFailure{ForegroundFailureReason::keyboard_state};
        if (needs_pointer && !g_pSeatManager->m_mouse) throw ForegroundFailure{ForegroundFailureReason::physical_pointer};
        const Vector2D local{x + c.geometry[0] - c.geometry[4], y + c.geometry[1] - c.geometry[5]};
        // This route focuses the window's own surface, so it refuses a point
        // where a popup or subsurface is drawn instead of pressing under it.
        if (needs_pointer && (!point(c, x, y) || pointer_hit_at(c, x, y).hit != PointerHit::root))
            throw ForegroundFailure{ForegroundFailureReason::pointer_target};
        const auto seat = g_pSeatManager->seatResourceForClient(root->client());
        if (!seat || !seat->good()) throw ForegroundFailure{ForegroundFailureReason::seat_resource};
        foreground_pointers.clear(); foreground_keyboards.clear();
        for (const auto& p : seat->m_pointers) if (p && p->good()) foreground_pointers.push_back(p);
        for (const auto& k : seat->m_keyboards) if (k && k->good()) foreground_keyboards.push_back(k);
        if (needs_pointer && foreground_pointers.empty()) throw ForegroundFailure{ForegroundFailureReason::pointer_resources};
        if (needs_keyboard && foreground_keyboards.empty())
            throw ForegroundFailure{ForegroundFailureReason::keyboard_resources};
        foreground_modifiers = {};
        if (needs_keyboard) {
            // Keep the human keyboard's modifiers and locks. The caller admitted
            // this chord under exactly this state before consuming the grant.
            foreground_modifiers = modifiers;
            xkb_state_update_mask(physical_keyboard_state, foreground_modifiers[0], foreground_modifiers[1],
                foreground_modifiers[2], 0, 0, foreground_modifiers[3]);
        }
        foreground_surface = root;
        foreground_seat = seat;
        foreground_needs_pointer = needs_pointer;
        foreground_needs_keyboard = needs_keyboard;
        c.foreground_attempted = true;
        foreground_started = true;
        foreground_activating = true;
        // Activation intentionally persists. Never save, borrow, or restore focus.
        if (g_pSeatManager->m_state.keyboardFocus != root || Desktop::focusState()->window() != c.window.lock() ||
            Desktop::focusState()->surface() != root)
            Desktop::focusState()->fullWindowFocus(c.window.lock(), Desktop::FOCUS_REASON_OTHER, root);
        foreground_activating = false;
        if (lease != &c) throw ForegroundFailure{ForegroundFailureReason::lease};
        const auto focus_failure = foreground_guard(c).dispatch_failure(false);
        if (focus_failure != ForegroundFailureReason::none) throw ForegroundFailure{focus_failure};
        if (!needs_pointer) { require_foreground(c); return; }
        foreground_activating = true;
        ::Pointer::mgr()->warpTo({x + c.geometry[0], y + c.geometry[1]});
        if (lease != &c) throw ForegroundFailure{ForegroundFailureReason::lease};
        const auto warp_failure = foreground_guard(c).dispatch_failure(false);
        if (warp_failure != ForegroundFailureReason::none) throw ForegroundFailure{warp_failure};
        g_pSeatManager->setPointerFocus(root, local);
        foreground_activating = false;
        require_foreground(c);
        foreground_motion(c, x, y);
    }
    void foreground_motion(Client& c, double x, double y) {
        require_foreground(c);
        const Vector2D local{x + c.geometry[0] - c.geometry[4], y + c.geometry[1] - c.geometry[5]};
        if (!point(c, x, y) || c.surface.lock()->at(local, true).first != c.surface.lock()) throw ForegroundFailure{ForegroundFailureReason::pointer_target};
        ::Pointer::mgr()->warpTo({x + c.geometry[0], y + c.geometry[1]});
        require_foreground(c);
        for (const auto& weak : foreground_pointers) {
            const auto p = weak.lock(); if (!p || !p->good()) throw ForegroundFailure{ForegroundFailureReason::pointer_resources};
            p->sendMotion(event_ms(), local); p->sendFrame();
        }
    }
    void foreground_button(Client& c, std::uint32_t code, bool pressed) {
        require_foreground(c);
        // Mark ownership before sending, so a later resource failure can unwind.
        if (pressed) held_button = code;
        for (const auto& weak : foreground_pointers) {
            const auto p = weak.lock(); if (!p || !p->good()) throw ForegroundFailure{ForegroundFailureReason::pointer_resources};
            p->sendButton(event_ms(), code, pressed ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED);
            p->sendFrame();
        }
        if (pressed) forget_press(foreground_seat, foreground_surface, code);
        held_button = pressed ? code : 0;
    }
    void foreground_key(Client& c, std::uint32_t code, bool pressed) {
        require_foreground(c);
        foreground_keyboard_used = true;
        xkb_state_update_key(physical_keyboard_state, code + 8, pressed ? XKB_KEY_DOWN : XKB_KEY_UP);
        if (pressed) held_keys.push_back(code); else std::erase(held_keys, code);
        for (const auto& weak : foreground_keyboards) {
            const auto k = weak.lock(); if (!k || !k->good()) throw ForegroundFailure{ForegroundFailureReason::keyboard_resources};
            k->sendKey(event_ms(), code, pressed ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED);
            k->sendMods(xkb_state_serialize_mods(physical_keyboard_state, XKB_STATE_MODS_DEPRESSED),
                xkb_state_serialize_mods(physical_keyboard_state, XKB_STATE_MODS_LATCHED),
                xkb_state_serialize_mods(physical_keyboard_state, XKB_STATE_MODS_LOCKED),
                xkb_state_serialize_layout(physical_keyboard_state, XKB_STATE_LAYOUT_EFFECTIVE));
        }
    }
    // `surface` is the window's own surface, one of its subsurfaces, or one of
    // its popups, and `local` the point in its coordinates.
    bool pointer_enter(Client& c, const SP<CWLSurfaceResource>& surface, const Vector2D& local) {
        const auto root = c.surface.lock(); if (!root || !surface) return false;
        // With xwayland:force_zero_scaling, X surfaces use X pixels. Hyprland
        // scales the human's pointer the same way.
        const auto window = c.window.lock();
        const auto sent = c.xwayland && window ? local * window->m_X11SurfaceScaledBy : local;
        unsigned count = 0;
        for (auto& p : pointers) {
            if (p->dead || !p->wl->resource() || p->wl->client() != surface->client()) continue;
            // XWayland places the pointer on enter without a raw event. A motion
            // event becomes XI2 raw motion, which every raw listener receives
            // (a relative-mouse game the human is playing turns it into a jump).
            // A drag still moves: XWayland releases held buttons on enter.
            const bool reenter = c.xwayland && !held_button;
            if (p->focus != surface || reenter) {
                if (const auto old = p->focus.lock(); old && old->good()) p->wl->sendLeave(serial(), old->getResource().get());
                p->focus = surface;
                p->wl->sendEnter(serial(), surface->getResource().get(), wl_fixed_from_double(sent.x), wl_fixed_from_double(sent.y));
            }
            if (!reenter) p->wl->sendMotion(event_ms(), wl_fixed_from_double(sent.x), wl_fixed_from_double(sent.y));
            if (p->wl->version() >= 5) p->wl->sendFrame();
            ++count;
        }
        if (count && !pointer_target.matches(c.window.lock(), root, c.geometry)) {
            pointer_unmap.reset(); pointer_destroy.reset();
            pointer_target.capture(c.window.lock(), root, c.geometry);
            // These listeners belong to the lane, never the socket Client.
            const auto window = c.window.lock();
            pointer_unmap = window->m_events.unmap.listen([this] { revoke("stale_target"); });
            pointer_destroy = window->m_events.destroy.listen([this] { revoke("stale_target"); });
        }
        return count > 0;
    }
    // A drag's motion goes to the window's own surface, where it started.
    // Popups are not in that surface's tree, so they never take it over. A
    // subsurface still ends the drag instead of taking motion meant for the window.
    bool drag_motion(Client& c, double x, double y) {
        const auto root = c.surface.lock(); if (!root) return false;
        const Vector2D local{x + c.geometry[0] - c.geometry[4], y + c.geometry[1] - c.geometry[5]};
        return root->at(local, true).first == root && pointer_enter(c, root, local);
    }
    void button(std::uint32_t value, bool pressed) {
        for (auto& p : pointers) {
            if (p->dead || !p->wl->resource() || !p->focus) continue;
            p->wl->sendButton(serial(), event_ms(), value, pressed ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED);
            if (p->wl->version() >= 5) p->wl->sendFrame();
        }
        held_button = pressed ? value : 0;
    }
    bool keyboard_enter(Client& c) {
        const auto root = c.surface.lock();
        if (!root || !keyboard_state) return false;
        unsigned count = 0;
        for (auto& k : keyboards) {
            if (k->dead || !k->wl->resource() || k->wl->client() != root->client()) continue;
            if (k->focus != root) {
                if (const auto old = k->focus.lock(); old && old->good()) k->wl->sendLeave(serial(), old->getResource().get());
                k->focus = root;
                wl_array keys{};
                k->wl->sendEnter(serial(), root->getResource().get(), &keys);
                k->wl->sendModifiers(serial(), 0, 0, 0, 0);
            }
            ++count;
        }
        return count > 0;
    }
    void key(std::uint32_t code, bool pressed) {
        xkb_state_update_key(keyboard_state, code + 8, pressed ? XKB_KEY_DOWN : XKB_KEY_UP);
        if (pressed) held_keys.push_back(code); else std::erase(held_keys, code);
        for (auto& k : keyboards) {
            if (k->dead || !k->wl->resource() || !k->focus) continue;
            k->wl->sendKey(serial(), event_ms(), code, pressed ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED);
            k->wl->sendModifiers(serial(), xkb_state_serialize_mods(keyboard_state, XKB_STATE_MODS_DEPRESSED),
                xkb_state_serialize_mods(keyboard_state, XKB_STATE_MODS_LATCHED),
                xkb_state_serialize_mods(keyboard_state, XKB_STATE_MODS_LOCKED),
                xkb_state_serialize_layout(keyboard_state, XKB_STATE_LAYOUT_EFFECTIVE));
        }
    }
    void request(Client& c, const std::vector<std::string>& f) {
        if (!foreground_started) c.foreground_attempted = false;
        sync_keymap();
        if (c.dead) return;
        const auto& command = f[0];
        if (command == "HELLO") {
            if (f.size() != 1 || c.hello) { send(c, refusal("invalid_request")); return; }
            c.hello = true;
            if (kProduction)
                send(c, std::format(R"({{"ok":true,"protocol":3,"epoch":"{}","foreground_target":true,"primary_background":true,"xwayland_target":true,"stable_id":true,"popup_input":true}})", epoch));
            else
                send(c, std::format(R"({{"ok":true,"protocol":0,"epoch":"{}","challenge":"{}"}})", epoch, c.challenge));
            return;
        }
        if (!c.hello) { send(c, refusal("invalid_request")); return; }
        if (command == "CLAIM" && f.size() == 1) {
            if (reservation && reservation != &c) { send(c, refusal("lane_busy")); return; }
            reservation = &c;
            send(c, std::format(R"({{"ok":true,"lane":{}}})", lane)); return;
        }
        if (command == "STOP" && f.size() == 1) {
            if (kProduction) {
                if (reservation != &c) { send(c, refusal("lane_not_claimed")); return; }
                cancel_authority("stopped");
            } else {
                for (auto* peer : peers) if (peer) peer->cancel_authority("stopped");
            }
            send(c, R"({"ok":true})"); return;
        }
        if (command == "CANCEL" && f.size() == 1) {
            if (kProduction && reservation != &c) { send(c, refusal("lane_not_claimed")); return; }
            cancel_authority("cancelled"); send(c, R"({"ok":true})"); return;
        }
        if (trace && ((command == "TRACE_START" || command == "TRACE_STOP") && f.size() == 1)) {
            send(c, trace->request(command)); return;
        }
        if (trace && command == "TRACE_READ" && f.size() == 2) {
            const auto after = number(f[1]);
            if (after > 32768) { send(c, refusal("invalid_request")); return; }
            send(c, trace->request(command, after)); return;
        }
#ifdef CUA_HYPRLAND_TEST_INPUT
        if (command == "APPROVE") { approve(c, f); return; }
#endif
        if (reservation != &c) { send(c, refusal("lane_not_claimed")); return; }
        if (command == "TARGET" || (kProduction && (command == "FOREGROUND_TARGET" || command == "PRIMARY_TARGET" ||
            command == "XWAYLAND_TARGET"))) {
            if (drag) { send(c, refusal("invalid_request")); return; }
            const auto route = command == "FOREGROUND_TARGET" ? InputRoute::primary_foreground :
                command == "PRIMARY_TARGET" ? InputRoute::primary_background : InputRoute::independent;
            if (!bind_input_route(c.route, route)) { invalidate(c); send(c, refusal("route_mismatch")); return; }
            if (kProduction ? !production_target_fields(f.size()) : f.size() != 3u) {
                if (kProduction) invalidate(c);
                send(c, refusal("invalid_request")); return;
            }
            // A new admission attempt always retires any unused old grant.
            // Do not leave an unchanged surface merely to refresh authority.
            if (kProduction) retire_grant();
            const auto requested_cap = kProduction ? number(f[3]) : 0;
            if (kProduction && (!InputGrant::single_operation(requested_cap) ||
                (requested_cap == 16 && route != InputRoute::primary_foreground))) { invalidate(c); send(c, refusal("unsupported")); return; }
            if (kProduction && !available()) { invalidate(c, false); send(c, refusal("session_unavailable")); return; }
            if (!input_layout_qualified(route, requested_cap == 2, layout_qualified())) { invalidate(c, false); send(c, refusal("unsupported_layout")); return; }
            const auto pid = number(f[1]); const auto address = number(f[2], 16);
            const auto stable_id = f.size() == 5 ? std::optional{number(f[4], 16)} : std::nullopt;
            PHLWINDOW window;
            for (const auto& w : Desktop::windowState()->windows())
                if (reinterpret_cast<std::uintptr_t>(w.get()) == address && static_cast<std::uint64_t>(w->getPID()) == pid) window = w;
            const bool xwayland = command == "XWAYLAND_TARGET";
            if (!window || !target_identity_matches(stable_id, window->m_stableID) || window->m_isX11 != xwayland ||
                !window->m_isMapped || window->isHidden() || !window->resource() || (xwayland && !window->m_xwaylandSurface)) {
                invalidate(c); send(c, refusal("stale_target")); return;
            }
            // Retire the old target before rebinding: even if the new target's
            // geometry validation fails, a reserved lane must not strand it.
            if (pointer_target.entered() && !pointer_target.same_target(window, window->resource()))
                revoke("stale_target");
            if (c.window != window || c.surface != window->resource() || c.xwayland != xwayland || c.token.empty()) {
                // Rebinding authority (including a new socket owner) may reuse
                // unchanged passive hover. Compare the actual target below.
                invalidate(c, false); c.unmap.reset(); c.destroy.reset();
                c.window = window; c.surface = window->resource(); c.xwayland = xwayland; c.token = nonce(); c.revision = 1;
                c.unmap = window->m_events.unmap.listen([this, &c] { invalidate(c); });
                c.destroy = window->m_events.destroy.listen([this, &c] { invalidate(c); });
            }
            if (!refresh(c)) { invalidate(c); send(c, refusal("stale_target")); return; }
            if (pointer_target.entered() && !pointer_target.matches(window, c.surface.lock(), c.geometry))
                revoke("stale_target");
            // Only a valid fresh TARGET can retire an orphan on another lane.
            // Dispatch/conflict checks never evict peer state or replay input.
            if (available() && !primary_conflict(c)) retire_orphan_hover(c);
            // Clients that bind one wl_seat never hold agent resources. Driver
            // uses this to select PRIMARY_TARGET explicitly; the plugin never
            // changes route on its own.
            const auto client = c.surface.lock()->client();
            const bool agent_bound = std::ranges::any_of(pointers, [&](const auto& p) {
                return !p->dead && p->wl->resource() && p->wl->client() == client;
            }) || std::ranges::any_of(keyboards, [&](const auto& k) {
                return !k->dead && k->wl->resource() && k->wl->client() == client;
            });
            if (xwayland && !agent_bound) { invalidate(c); send(c, refusal("client_not_bound")); return; }
            if (kProduction) {
                if (primary_conflict(c)) { invalidate(c); send(c, refusal("primary_target_busy")); return; }
                if (agent_conflict(c)) { invalidate(c); send(c, refusal("agent_target_busy")); return; }
                if (const auto failure = keyboard_conflict(c, requested_cap); !failure.empty()) {
                    invalidate(c, false); send(c, refusal(failure)); return;
                }
                if (route == InputRoute::primary_foreground) {
                    const auto failure = foreground_guard(c).activation_failure();
                    if (failure != ForegroundFailureReason::none) {
                        invalidate(c); send(c, refusal("primary_target_busy", ForegroundFailure{failure})); return;
                    }
                }
                if (route == InputRoute::primary_background) {
                    if (const auto failure = primary_background_guard(c).failure(); !failure.empty()) {
                        invalidate(c); send(c, refusal(failure)); return;
                    }
                }
                if (uses_primary_seat(route)) leave_pointer();
                c.foreground_attempted = false;
                c.token = nonce();
                grant.arm(requested_cap, Clock::now());
                lease = &c; capabilities = requested_cap; expires = grant.deadline();
                if (trace) trace->mark("agent_admitted", lane + 1);
            }
            c.has_bound_target = true;
            if (route == InputRoute::primary_foreground) {
                send(c, std::format(R"({{"ok":true,"target":"{}","revision":{},"width":{},"height":{},"route":"primary_foreground"}})",
                    c.token, c.revision, c.geometry[2], c.geometry[3])); return;
            }
            if (route == InputRoute::primary_background) {
                send(c, std::format(R"({{"ok":true,"target":"{}","revision":{},"width":{},"height":{},"route":"primary_background"}})",
                    c.token, c.revision, c.geometry[2], c.geometry[3])); return;
            }
            if (xwayland) {
                c.human_focus = g_pSeatManager->m_state.keyboardFocus;
                // XWayland names its devices for a seat "xwayland-<kind>:<global name>".
                send(c, std::format(R"({{"ok":true,"target":"{}","revision":{},"width":{},"height":{},"route":"xwayland","seat":{},"xid":{}}})",
                    c.token, c.revision, c.geometry[2], c.geometry[3], wl_global_get_name(global, client),
                    window->m_xwaylandSurface->m_xID)); return;
            }
            send(c, std::format(R"({{"ok":true,"target":"{}","revision":{},"width":{},"height":{},"agent_bound":{}}})",
                c.token, c.revision, c.geometry[2], c.geometry[3], agent_bound)); return;
        }
        const std::uint64_t cap = command == "CLICK" ? 1 : command == "KEY" ? 2 : command == "SCROLL" ? 4 : command == "DRAG" ? 8 :
            command == "ACTIVATE" && kProduction && c.route == InputRoute::primary_foreground ? 16 : 0;
        const std::size_t count = command == "CLICK" ? 8 : command == "KEY" ? 6 : command == "SCROLL" ? 8 : command == "ACTIVATE" ? 4 : 9;
        if (!cap) { send(c, refusal("unsupported")); return; }
        if (f.size() != count) { send(c, refusal("invalid_request")); return; }
        const auto sequence = number(f[1]);
        if (sequence <= c.sequence) { send(c, refusal("replay")); return; }
        c.sequence = sequence;
        if (c.token.empty() || f[2] != c.token || !refresh(c)) { send(c, refusal("stale_target")); return; }
        if (number(f[3]) != c.revision) { if (kProduction) revoke("stale_geometry"); send(c, refusal("stale_geometry")); return; }
        if (!available()) { revoke("session_unavailable", true); send(c, refusal("session_unavailable")); return; }
        if (!input_layout_qualified(c.route, cap == 2, layout_qualified())) { revoke("unsupported_layout", true); send(c, refusal("unsupported_layout")); return; }
        if (lease && Clock::now() >= expires) revoke("lease_expired");
        if (drag) { send(c, refusal("lease_busy")); return; }
        if (lease != &c || !(capabilities & cap) || (kProduction && !grant.permits(cap, Clock::now()))) {
            if (kProduction) {
                revoke("action_not_admitted");
                send(c, refusal("action_not_admitted"));
            } else {
                send(c, std::format(R"({{"ok":false,"code":"pending_operator_approval","detail":"external test operator approval required","epoch":"{}","challenge":"{}","target":"{}"}})", epoch, c.challenge, c.token));
            }
            return;
        }
        if (primary_conflict(c)) {
            if (kProduction) revoke("primary_target_busy");
            send(c, refusal("primary_target_busy")); return;
        }
        if (agent_conflict(c)) {
            if (kProduction) revoke("agent_target_busy");
            send(c, refusal("agent_target_busy")); return;
        }
        // Driver sends each key of a text as its own TARGET and KEY, so this
        // runs before every key. The person's focus can reach the client only
        // between requests: KEY enters, types, and leaves in this one handler.
        if (const auto failure = keyboard_conflict(c, cap); !failure.empty()) {
            if (kProduction) revoke(failure, true);
            send(c, refusal(failure)); return;
        }
        // Hyprland raises an X window in X stacking when it gains keyboard
        // focus, and X stacking decides which window a pointer event reaches.
        // Driver checks coverage after XWAYLAND_TARGET, so a focus change since
        // then voids that check.
        if (c.xwayland && cap != 2 && g_pSeatManager->m_state.keyboardFocus.lock() != c.human_focus.lock()) {
            send(c, refusal("xwayland_focus_changed")); return;
        }
        // Every X raw listener sees agent pointer motion, and a game that locks
        // or confines the human's pointer usually reads exactly that. Clicks
        // and scrolls send no motion, so only a drag waits for the game.
        if (c.xwayland && cap == 8 && g_pInputManager->isConstrained()) {
            send(c, refusal("primary_constraint")); return;
        }
        if (c.route == InputRoute::primary_foreground) {
            foreground_request(c, f, cap);
            return;
        }
        if (c.route == InputRoute::primary_background) {
            primary_background_request(c, f, cap);
            return;
        }
        if (command == "KEY") {
            const auto code = number(f[4]); const auto mods = number(f[5]);
            if (code == 0 || code > 247 || mods > 15 || code == 58 || code == 69 || code == 70) { send(c, refusal("unsupported")); return; }
            if (!consume_grant(c, cap)) return;
            if (!keyboard_enter(c)) { send(c, refusal("client_not_bound")); return; }
            // Every key is released in this handler; sync_key_repeat relies on it.
            const std::array<std::uint32_t, 4> keys{42, 29, 56, 125};
            for (unsigned i = 0; i < 4; ++i) if ((mods & (1u << i)) && keys[i] != code) key(keys[i], true);
            key(code, true); key(code, false);
            for (int i = 3; i >= 0; --i) if ((mods & (1u << i)) && keys[i] != code) key(keys[i], false);
        } else {
            const auto x = real(f[4]), y = real(f[5]);
            const auto target = pointer_target_at(c, x, y, command == "DRAG");
            if (!target) return;
            if (command == "CLICK") {
                const auto btn = number(f[6]), clicks = number(f[7]);
                if (btn < 272 || btn > 274 || clicks < 1 || clicks > 2) { send(c, refusal("invalid_request")); return; }
                if (!consume_grant(c, cap)) return;
                if (!pointer_enter(c, target->surface, target->local)) { send(c, refusal("client_not_bound")); return; }
                for (unsigned i = 0; i < clicks; ++i) { button(btn, true); button(btn, false); }
            } else if (command == "SCROLL") {
                const auto axis = number(f[6]); const auto value = real(f[7]);
                if (axis > 1 || value == 0 || std::abs(value) > 1000) { send(c, refusal("invalid_request")); return; }
                if (!consume_grant(c, cap)) return;
                if (!pointer_enter(c, target->surface, target->local)) { send(c, refusal("client_not_bound")); return; }
                for (auto& p : pointers) {
                    if (p->dead || !p->wl->resource() || !p->focus) continue;
                    const auto wheel = [&](double amount) {
                        if (p->wl->version() >= 5) p->wl->sendAxisSource(WL_POINTER_AXIS_SOURCE_WHEEL);
                        p->wl->sendAxis(event_ms(), static_cast<wl_pointer_axis>(axis), wl_fixed_from_double(amount));
                        if (p->wl->version() >= 5) p->wl->sendFrame();
                    };
                    // XI2 clients take the first scroll after an enter as their
                    // baseline. With no motion to set it, an empty scroll does.
                    if (c.xwayland) wheel(0.0);
                    wheel(value);
                }
            } else {
                const auto x2 = real(f[6]), y2 = real(f[7]); const auto duration = number(f[8]);
                if (!point(c, x2, y2)) { send(c, refusal("point_outside_window")); return; }
                if (duration < 50 || duration > 2000) { send(c, refusal("invalid_request")); return; }
                if (Clock::now() + std::chrono::milliseconds(duration + 50) >= expires) { send(c, refusal("lease_expired")); return; }
                if (!consume_grant(c, cap)) return;
                if (!pointer_enter(c, target->surface, target->local)) { send(c, refusal("client_not_bound")); return; }
                if (trace) trace->mark("agent_drag_start", lane + 1);
                button(272, true);
                drag.emplace(Drag{&c, x, y, x2, y2, Clock::now(), static_cast<unsigned>(duration), DragGeometry{c.revision}});
                send(c, R"({"ok":true,"phase":"started"})"); return;
            }
        }
        ++dispatches;
        if (trace) trace->mark("agent_action_end", lane + 1);
        note_dispatch(c);
        if (kProduction) complete_action();
        send(c, kDelivered);
    }
    void foreground_request(Client& c, const std::vector<std::string>& f, std::uint64_t cap) {
        const auto& command = f[0];
        double x = c.geometry[2] / 2, y = c.geometry[3] / 2, x2 = 0, y2 = 0, value = 0;
        std::uint64_t code = 0, mods = 0, clicks = 0, axis = 0, duration = 0;
        if (command == "KEY") {
            code = number(f[4]); mods = number(f[5]);
            if (code == 0 || code > 247 || mods > 15 || code == 58 || code == 69 || code == 70) {
                send(c, refusal("unsupported")); return;
            }
        } else if (command != "ACTIVATE") {
            x = real(f[4]); y = real(f[5]);
            if (!point(c, x, y)) { send(c, refusal("point_outside_window")); return; }
            if (command == "CLICK") {
                code = number(f[6]); clicks = number(f[7]);
                if (code < 272 || code > 274 || clicks < 1 || clicks > 2) { send(c, refusal("invalid_request")); return; }
            } else if (command == "SCROLL") {
                axis = number(f[6]); value = real(f[7]);
                if (axis > 1 || value == 0 || std::abs(value) > 1000) { send(c, refusal("invalid_request")); return; }
            } else {
                x2 = real(f[6]); y2 = real(f[7]); duration = number(f[8]);
                if (!point(c, x2, y2)) { send(c, refusal("point_outside_window")); return; }
                if (duration < 50 || duration > 2000) { send(c, refusal("invalid_request")); return; }
                if (Clock::now() + std::chrono::milliseconds(duration + 50) >= expires) { send(c, refusal("lease_expired")); return; }
            }
        }
        std::array<std::uint32_t, 4> modifiers{};
        if (command == "KEY") {
            // Simulate the whole chord under the human keyboard's live state
            // before consuming the grant or changing focus. Num Lock and Caps
            // Lock stay as they are; a chord whose meaning they or the layout
            // would change refuses with its specific reason.
            modifiers = capture_foreground_modifiers(true);
            const auto failure = foreground_chord_failure(physical_keymap, keymap, static_cast<std::uint32_t>(code),
                static_cast<std::uint32_t>(mods), modifiers);
            if (failure != ForegroundFailureReason::none) throw ForegroundFailure{failure};
        }
        if (!consume_grant(c, cap)) return;
        start_foreground(c, x, y, command != "KEY" && command != "ACTIVATE", command == "KEY", modifiers);
        if (command == "KEY") {
            const std::array<std::uint32_t, 4> keys{42, 29, 56, 125};
            for (unsigned i = 0; i < 4; ++i) if ((mods & (1u << i)) && keys[i] != code) foreground_key(c, keys[i], true);
            foreground_key(c, code, true); foreground_key(c, code, false);
            for (int i = 3; i >= 0; --i) if ((mods & (1u << i)) && keys[i] != code) foreground_key(c, keys[i], false);
        } else if (command == "CLICK") {
            for (unsigned i = 0; i < clicks; ++i) { foreground_button(c, code, true); foreground_button(c, code, false); }
        } else if (command == "SCROLL") {
            require_foreground(c);
            for (const auto& weak : foreground_pointers) {
                const auto p = weak.lock(); if (!p || !p->good()) throw ForegroundFailure{ForegroundFailureReason::pointer_resources};
                p->sendAxisSource(WL_POINTER_AXIS_SOURCE_WHEEL);
                p->sendAxis(event_ms(), static_cast<wl_pointer_axis>(axis), value); p->sendFrame();
            }
        } else if (command == "DRAG") {
            foreground_button(c, 272, true);
            drag.emplace(Drag{&c, x, y, x2, y2, Clock::now(), static_cast<unsigned>(duration), DragGeometry{c.revision}});
            send(c, R"({"ok":true,"phase":"started"})"); return;
        }
        require_foreground(c);
        ++dispatches;
        complete_action();
        send(c, kForegroundDelivered);
    }
    // Background delivery through the target's primary-seat resources. Click,
    // key, and scroll borrow, send, and return within this one handler, so the
    // compositor cannot interleave a human event or focus change mid-action.
    void primary_background_request(Client& c, const std::vector<std::string>& f, std::uint64_t cap) {
        const auto& command = f[0];
        if (command == "ACTIVATE") { send(c, refusal("unsupported")); return; }
        if (const auto failure = primary_background_guard(c).failure(); !failure.empty()) {
            revoke(failure); send(c, refusal(failure)); return;
        }
        const auto root = c.surface.lock();
        double x = 0, y = 0;
        std::optional<PointerSurfaceAt> target;
        if (command != "KEY") {
            x = real(f[4]); y = real(f[5]);
            target = pointer_target_at(c, x, y, command == "DRAG");
            if (!target) return;
            // A point where no surface of the window takes input.
            if (!target->surface) { send(c, refusal("subsurface_target")); return; }
        }
        if (command == "KEY") {
            const auto code = number(f[4]); const auto mods = number(f[5]);
            if (code == 0 || code > 247 || mods > 15 || code == 58 || code == 69 || code == 70) { send(c, refusal("unsupported")); return; }
            // The client decodes with the human's keymap but only the neutral
            // modifier state we send it; the human keyboard's locks never
            // reach a client that the human has not focused.
            if (foreground_chord_failure(physical_keymap, keymap, static_cast<std::uint32_t>(code),
                    static_cast<std::uint32_t>(mods), {}) != ForegroundFailureReason::none) {
                send(c, refusal("unsupported_layout")); return;
            }
            if (!consume_grant(c, cap)) return;
            borrow_primary(c, false, true);
            if (borrowed_keyboards.empty() || !physical_keyboard_state) { return_primary(); send(c, refusal("client_not_bound")); return; }
            xkb_state_update_mask(physical_keyboard_state, 0, 0, 0, 0, 0, 0);
            wl_array keys{};
            for (const auto& weak : borrowed_keyboards)
                if (const auto k = weak.lock(); k && k->good()) { k->sendEnter(root, &keys); k->sendMods(0, 0, 0, 0); }
            const std::array<std::uint32_t, 4> chord{42, 29, 56, 125};
            for (unsigned i = 0; i < 4; ++i) if ((mods & (1u << i)) && chord[i] != code) borrowed_key(chord[i], true);
            borrowed_key(static_cast<std::uint32_t>(code), true); borrowed_key(static_cast<std::uint32_t>(code), false);
            for (int i = 3; i >= 0; --i) if ((mods & (1u << i)) && chord[i] != code) borrowed_key(chord[i], false);
        } else if (command == "CLICK") {
            const auto btn = number(f[6]), clicks = number(f[7]);
            if (btn < 272 || btn > 274 || clicks < 1 || clicks > 2) { send(c, refusal("invalid_request")); return; }
            if (!consume_grant(c, cap)) return;
            borrow_primary(c, true, false);
            if (borrowed_pointers.empty()) { return_primary(); send(c, refusal("client_not_bound")); return; }
            borrowed_pointer_enter(target->surface, target->local);
            for (unsigned i = 0; i < clicks; ++i) { borrowed_button(static_cast<std::uint32_t>(btn), true); borrowed_button(static_cast<std::uint32_t>(btn), false); }
        } else if (command == "SCROLL") {
            const auto axis = number(f[6]); const auto value = real(f[7]);
            if (axis > 1 || value == 0 || std::abs(value) > 1000) { send(c, refusal("invalid_request")); return; }
            if (!consume_grant(c, cap)) return;
            borrow_primary(c, true, false);
            if (borrowed_pointers.empty()) { return_primary(); send(c, refusal("client_not_bound")); return; }
            borrowed_pointer_enter(target->surface, target->local);
            for (const auto& weak : borrowed_pointers)
                if (const auto p = weak.lock(); p && p->good()) {
                    p->sendAxisSource(WL_POINTER_AXIS_SOURCE_WHEEL);
                    p->sendAxis(event_ms(), static_cast<wl_pointer_axis>(axis), value); p->sendFrame();
                }
        } else {
            const auto x2 = real(f[6]), y2 = real(f[7]); const auto duration = number(f[8]);
            if (!point(c, x2, y2)) { send(c, refusal("point_outside_window")); return; }
            if (duration < 50 || duration > 2000) { send(c, refusal("invalid_request")); return; }
            if (Clock::now() + std::chrono::milliseconds(duration + 50) >= expires) { send(c, refusal("lease_expired")); return; }
            if (!consume_grant(c, cap)) return;
            borrow_primary(c, true, false);
            if (borrowed_pointers.empty()) { return_primary(); send(c, refusal("client_not_bound")); return; }
            borrowed_pointer_enter(target->surface, target->local);
            borrowed_button(272, true);
            drag.emplace(Drag{&c, x, y, x2, y2, Clock::now(), static_cast<unsigned>(duration), DragGeometry{c.revision}});
            send(c, R"({"ok":true,"phase":"started"})"); return;
        }
        return_primary();
        ++dispatches;
        note_dispatch(c);
        complete_action();
        send(c, kPrimaryBackgroundDelivered);
    }
    void primary_background_motion(Client& c, double x, double y) {
        if (!borrowed_live(c)) { revoke("cancelled"); return; }
        const Vector2D local{x + c.geometry[0] - c.geometry[4], y + c.geometry[1] - c.geometry[5]};
        for (const auto& weak : borrowed_pointers)
            if (const auto p = weak.lock(); p && p->good()) { p->sendMotion(event_ms(), local); p->sendFrame(); }
    }
    bool consume_grant(Client& c, std::uint64_t capability) {
        if (!kProduction) return true;
        capabilities = 0;
        if (grant.consume(capability, Clock::now())) return true;
        revoke("action_not_admitted");
        send(c, refusal("action_not_admitted"));
        return false;
    }
    static int tick(void* data) {
        auto& self = *static_cast<Impl*>(data);
        try { self.step(); } catch (...) { self.revoke("internal_error"); }
        wl_event_source_timer_update(self.timer, self.retired ? 500 : 16); return 0;
    }
    void guard_targets() {
        if (lease) {
            const bool keyboard_action = (capabilities & 2) != 0 || foreground_needs_keyboard;
            if (lease->dead) revoke("disconnected", true);
            else if (Clock::now() >= expires) revoke("lease_expired");
            else if (!available() || !input_layout_qualified(lease->route, keyboard_action, layout_qualified()))
                revoke("cancelled", true);
            else if (!refresh(*lease) || primary_conflict(*lease) || agent_conflict(*lease) ||
                (drag && !drag->geometry.matches(lease->revision))) revoke("cancelled");
        }
        if (pointer_target.entered()) {
            const auto surface = pointer_target.surface();
            const auto window = pointer_target.window();
            const auto geometry = target_geometry(window, surface, window && window->m_isX11);
            // Hover rests where the last action entered: the window's own
            // surface, one of its subsurfaces, or one of its popups. A popup
            // that closes takes the hover with it.
            const bool bound = std::ranges::any_of(pointers, [&](const auto& p) {
                const auto focus = p->focus.lock();
                return !p->dead && p->wl->resource() && surface && focus && focus->m_mapped;
            });
            // Availability controls admission, not passive pointer presence.
            // Retention never excuses a changed target, binding, or conflict.
            if (!geometry || *geometry != pointer_target.geometry() ||
                !bound || agent_conflict(surface)) revoke("cancelled");
        }
    }
    void step() {
        if (!retired) sync_keymap();
        for (auto& c : clients) if (c->deadline.expired(c->hello, Clock::now())) c->dead = true;
        guard_targets();
        if (drag) {
            const auto d = *drag;
            const auto elapsed = std::chrono::duration<double, std::milli>(Clock::now() - d.start).count();
            const auto progress = std::min(elapsed / d.duration, 1.0);
            if (d.client->route == InputRoute::primary_foreground) {
                foreground_motion(*d.client, d.x1 + (d.x2 - d.x1) * progress, d.y1 + (d.y2 - d.y1) * progress);
                if (progress >= 1) {
                    foreground_button(*d.client, 272, false); drag.reset(); ++dispatches;
                    complete_action(); send(*d.client, kForegroundDelivered);
                }
            } else if (d.client->route == InputRoute::primary_background) {
                primary_background_motion(*d.client, d.x1 + (d.x2 - d.x1) * progress, d.y1 + (d.y2 - d.y1) * progress);
                if (drag && progress >= 1) {
                    borrowed_button(272, false); drag.reset(); return_primary(); ++dispatches;
                    note_dispatch(*d.client); complete_action(); send(*d.client, kPrimaryBackgroundDelivered);
                }
            } else if (!drag_motion(*d.client, d.x1 + (d.x2 - d.x1) * progress, d.y1 + (d.y2 - d.y1) * progress)) revoke("client_not_bound");
            else if (progress >= 1) {
                button(272, false); drag.reset(); ++dispatches;
                if (trace) trace->mark("agent_drag_end", lane + 1);
                note_dispatch(*d.client);
                if (kProduction) complete_action();
                send(*d.client, kDelivered);
            }
        }
        reap_clients();
        std::erase_if(pointers, [](auto& p) { return p->dead; });
        std::erase_if(keyboards, [](auto& k) { return k->dead; });
        std::erase_if(touches, [](auto& t) { return t->dead; });
        std::erase_if(seats, [](auto& s) { return s->dead; });
    }
};

// Hyprland 0.56.2 signals run synchronously on the compositor thread. Keep
// their ownership explicit and detach before retiring the seats. No polling
// or aggregate-state comparison substitutes for this transition boundary.
struct InputExperiment::DesktopListeners {
    struct MonitorListeners {
        PHLMONITORREF monitor;
        CHyprSignalListener dpms, mode;
    };
    InputExperiment& owner;
    std::vector<MonitorListeners> monitors;
    CHyprSignalListener lock, unlock, active, layout, added, removed, destroyed;
    CHyprSignalListener keyboard_layout, pointer_focus, keyboard_focus;
    CHyprSignalListener mouse_move, mouse_button, mouse_axis, keyboard_key, touch_down, tablet_tip;

    explicit DesktopListeners(InputExperiment& input) : owner(input) {
        lock = g_pSessionLockManager->m_events.lock.listen([this] { changed(); });
        unlock = g_pSessionLockManager->m_events.unlock.listen([this] { changed(); });
        if (g_pCompositor->m_aqBackend->hasSession())
            active = g_pCompositor->m_aqBackend->session->events.changeActive.listen([this] { changed(); });
        layout = Event::bus()->m_events.monitor.layoutChanged.listen([this] { changed(); });
        if (kProduction) {
            keyboard_layout = Event::bus()->m_events.input.keyboard.layout.listen(
                [this](SP<IKeyboard>, const std::string&) { changed(); });
            pointer_focus = g_pSeatManager->m_events.pointerFocusChange.listen([this] { primary_changed(); });
            keyboard_focus = g_pSeatManager->m_events.keyboardFocusChange.listen([this] { primary_changed(); });
            // These notifications precede physical delivery. Retire our owned
            // press before the user's event can become held on the same resource.
            mouse_move = Event::bus()->m_events.input.mouse.move.listen([this](Vector2D, Event::SCallbackInfo&) { external_input(); });
            mouse_button = Event::bus()->m_events.input.mouse.button.listen([this](IPointer::SButtonEvent, Event::SCallbackInfo&) { external_input(); });
            mouse_axis = Event::bus()->m_events.input.mouse.axis.listen([this](IPointer::SAxisEvent, Event::SCallbackInfo&) { external_input(); });
            keyboard_key = Event::bus()->m_events.input.keyboard.key.listen([this](IKeyboard::SKeyEvent, Event::SCallbackInfo&) { external_input(); });
            touch_down = Event::bus()->m_events.input.touch.down.listen([this](ITouch::SDownEvent, Event::SCallbackInfo&) { external_input(); });
            tablet_tip = Event::bus()->m_events.input.tablet.tip.listen([this](CTablet::STipEvent, Event::SCallbackInfo&) { external_input(); });
        }
        added = Event::bus()->m_events.monitor.preAdded.listen([this](PHLMONITOR monitor) {
            changed(); watch(monitor);
        });
        removed = Event::bus()->m_events.monitor.preRemoved.listen([this](PHLMONITOR) { changed(); });
        destroyed = Event::bus()->m_events.monitor.destroyMon.listen([this](PHLMONITOR monitor) {
            changed();
            std::erase_if(monitors, [&](const auto& entry) { return !entry.monitor || entry.monitor == monitor; });
        });
        for (const auto& monitor : State::monitorState()->allMonitors()) watch(monitor);
    }
    void changed() {
        for (auto& lane : owner.lanes_) lane->desktop_transition();
    }
    void external_input() {
        for (auto& lane : owner.lanes_)
            if (lane->lease && lane->lease->route == InputRoute::primary_foreground)
                lane->cancel_authority("foreground_interrupted", false);
    }
    void primary_changed() {
        for (auto& lane : owner.lanes_) {
            if (lane->lease && lane->primary_conflict(*lane->lease))
                lane->cancel_authority("primary_target_busy", false);
        }
    }
    void watch(PHLMONITOR monitor) {
        if (!monitor || std::ranges::any_of(monitors, [&](const auto& entry) { return entry.monitor == monitor; })) return;
        MonitorListeners listeners;
        listeners.monitor = monitor;
        listeners.dpms = monitor->m_events.dpmsChanged.listen([this] { changed(); });
        listeners.mode = monitor->m_events.modeChanged.listen([this] { changed(); });
        monitors.push_back(std::move(listeners));
    }
};

static CFunctionHook* install_hook(void* plugin, const std::string& symbol, void* destination) {
    // Exact mangled names pin these hooks to the audited 0.56.2 signatures.
    const auto matches = HyprlandAPI::findFunctionsByName(plugin, symbol);
    if (matches.size() != 1) throw std::runtime_error("input hook target unavailable");
    auto* hook = HyprlandAPI::createFunctionHook(plugin, matches.front().address, destination);
    if (!hook || !hook->hook()) throw std::runtime_error("input hook refused");
    return hook;
}

// Popup grabs and drag starts are client requests the compositor accepts from
// any seat, and Hyprland focuses and raises every new window. These hooks and
// listeners keep a client reacting to agent input from taking the human's
// focus or pointer. They forward everything else unchanged.
struct InputExperiment::ReactionGuards {
    using Grab = void (*)(CXDGShellProtocol*, SP<CXDGPopupResource>);
    using Drag = void (*)(CWLDataDeviceProtocol*, WP<CWLDataSourceResource>, SP<CWLSurfaceResource>, SP<CWLSurfaceResource>);
    static inline ReactionGuards* live = nullptr;
    InputExperiment& owner;
    void* plugin;
    CFunctionHook* grab = nullptr;
    CFunctionHook* drag = nullptr;
    std::uint64_t refused_grabs = 0, refused_drags = 0, withheld_focus = 0;
    CHyprSignalListener open_early, open, human_key, human_button, human_focus;
    // The reaction window between its openEarly and open events.
    PHLWINDOWREF withheld;
    // Hyprland drops the seat and serial of xdg_popup.grab and
    // wl_data_device.start_drag. The protocol logger sees each request just
    // before its handler runs, so the hooks read them from here.
    wl_protocol_logger* logger = nullptr;
    struct Quote {
        wl_client* client = nullptr;
        wl_resource* seat = nullptr;
        std::uint32_t serial = 0;
    } quoted;
    std::vector<HumanCommand> human_commands;
    // Popups whose grab was withheld. GTK and Qt send every key of an app to
    // its open menu, wherever the compositor sends them, so such a menu
    // closes as soon as the human's keyboard is in its app. Closing it at
    // their first key is too late: GTK still hands that key to the menu.
    std::vector<WP<CXDGPopupResource>> agent_popups;

    ReactionGuards(InputExperiment& input, void* handle) : owner(input), plugin(handle) {
        if (live) throw std::runtime_error("input reaction guards already installed");
        live = this;
        // A throwing constructor never runs the destructor.
        try {
            grab = install_hook(plugin, "_ZN17CXDGShellProtocol14addOrStartGrabEN9Hyprutils6Memory14CSharedPointerI17CXDGPopupResourceEE",
                reinterpret_cast<void*>(guarded_grab));
            drag = install_hook(plugin, "_ZN21CWLDataDeviceProtocol12initiateDragEN9Hyprutils6Memory12CWeakPointerI21CWLDataSourceResourceEENS1_14CSharedPointerI18CWLSurfaceResourceEES7_",
                reinterpret_cast<void*>(guarded_drag));
            logger = wl_display_add_protocol_logger(g_pCompositor->m_wlDisplay, quote_request, this);
            if (!logger) throw std::runtime_error("input protocol logger unavailable");
            // Both fire before Hyprland delivers the human's input.
            human_key = Event::bus()->m_events.input.keyboard.key.listen([this](IKeyboard::SKeyEvent e, Event::SCallbackInfo&) {
                if (e.state != WL_KEYBOARD_KEY_STATE_PRESSED) return;
                const auto focus = g_pSeatManager->m_state.keyboardFocus.lock();
                close_agent_popups(focus);
                if (g_pInputManager->getModsFromAllKBs() & (HL_MODIFIER_CTRL | HL_MODIFIER_ALT | HL_MODIFIER_META)) note_command(focus);
            });
            human_button = Event::bus()->m_events.input.mouse.button.listen([this](IPointer::SButtonEvent e, Event::SCallbackInfo&) {
                if (e.state != WL_POINTER_BUTTON_STATE_PRESSED) return;
                const auto focus = g_pSeatManager->m_state.pointerFocus.lock();
                close_agent_popups(focus);
                note_command(focus);
            });
            human_focus = g_pSeatManager->m_events.keyboardFocusChange.listen([this] { close_agent_popups(g_pSeatManager->m_state.keyboardFocus.lock()); });
            // Hyprland focuses a new window after openEarly unless this is set, and
            // sets it itself for X menus that declare their type.
            open_early = Event::bus()->m_events.window.openEarly.listen([this](PHLWINDOW window) {
                if (!window || window->m_noInitialFocus) return;
                const auto anchor = anchor_of(window);
                const auto replaced = anchor ? nullptr : replaced_app(window);
                if (!anchor && !replaced) return;
                window->m_noInitialFocus = true;
                // Otherwise it would join the human's focused group as its visible tab.
                window->m_groupRules |= Desktop::View::GROUP_BARRED;
                open_on(window, anchor ? anchor->m_workspace : workspace_of(*replaced));
                const bool visible = window->m_workspace && window->m_workspace->isVisible();
                window->m_isFloating = reaction_floats(window->m_isFloating, visible, Fullscreen::controller()->hasFullscreen(window->m_workspace));
                // A floating window still draws over tiled ones and may open under
                // the human's pointer. With follow_mouse, merely moving the pointer
                // would then hand it their keyboard. A click still does.
                if (visible) window->m_ruleApplicator->noFollowMouse().set(true, Desktop::Types::PRIORITY_SET_PROP);
                withheld = window;
                ++withheld_focus;
            });
            // Between these events Hyprland raises a new floating window and lets
            // it over a fullscreen window.
            open = Event::bus()->m_events.window.open.listen([this](PHLWINDOW window) {
                if (!window || window != withheld.lock()) return;
                withheld.reset();
                keep_below_human(window);
            });
        } catch (...) {
            uninstall();
            throw;
        }
    }
    ~ReactionGuards() { uninstall(); }
    void uninstall() {
        if (drag) HyprlandAPI::removeFunctionHook(plugin, drag);
        if (grab) HyprlandAPI::removeFunctionHook(plugin, grab);
        if (logger) wl_protocol_logger_destroy(logger);
        live = nullptr;
    }
    static void quote_request(void* data, wl_protocol_logger_type type, const wl_protocol_logger_message* m) {
        if (type != WL_PROTOCOL_LOGGER_REQUEST) return;
        auto& guards = *static_cast<ReactionGuards*>(data);
        const std::string_view name = m->message->name, interface = wl_resource_get_class(m->resource);
        // xdg_popup.grab(seat, serial) and wl_data_device.start_drag(source, origin, icon, serial).
        if (name == "grab" && interface == "xdg_popup" && m->arguments_count == 2)
            guards.quoted = {wl_resource_get_client(m->resource), reinterpret_cast<wl_resource*>(m->arguments[0].o), m->arguments[1].u};
        else if (name == "start_drag" && interface == "wl_data_device" && m->arguments_count == 4)
            guards.quoted = {wl_resource_get_client(m->resource), nullptr, m->arguments[3].u};
    }
    // Closes the agent's menus in the app the human's input is going to,
    // unless that input is on one of them.
    void close_agent_popups(const SP<CWLSurfaceResource>& surface) {
        const auto surface_of = [](const SP<CXDGPopupResource>& popup) {
            return popup && popup->m_surface ? popup->m_surface->m_surface.lock() : nullptr;
        };
        std::erase_if(agent_popups, [&](const auto& weak) { return !surface_of(weak.lock()); });
        if (!surface || std::ranges::any_of(agent_popups, [&](const auto& weak) { return surface_of(weak.lock()) == surface; })) return;
        std::erase_if(agent_popups, [&](const auto& weak) {
            const auto popup = weak.lock();
            if (surface_of(popup)->client() != surface->client()) return false;
            popup->done();
            return true;
        });
    }
    void note_command(const SP<CWLSurfaceResource>& surface) {
        if (!surface) return;
        const auto now = Clock::now();
        std::erase_if(human_commands, [&](const auto& c) { return now - c.at >= kReactionWindow || !c.surface; });
        human_commands.push_back({surface, now});
    }
    // Whether a popup grab or drag start from `surface`, in the window whose
    // own surface is `root`, reacts to agent input. Reads and clears the quote.
    bool agent_input(const SP<CWLSurfaceResource>& surface, const SP<CWLSurfaceResource>& root) {
        GrabSignals signals;
        if (quoted.client == surface->client()) {
            signals.agent_seat = quoted.seat && std::ranges::any_of(owner.lanes_, [&](const auto& lane) {
                return std::ranges::any_of(lane->seats, [&](const auto& seat) { return !seat->dead && seat->wl->resource() == quoted.seat; });
            });
            signals.agent_serial = std::ranges::any_of(owner.lanes_, [&](const auto& lane) { return lane->agent_serials.contains(quoted.serial); });
        }
        quoted = {};
        if (root) {
            std::optional<Clock::time_point> human, agent;
            for (const auto& c : human_commands)
                if (const auto s = c.surface.lock(); s && (s == root || root->findFirstPreorder([&](const auto& sub) { return sub == s; })) && (!human || c.at > *human))
                    human = c.at;
            for (const auto& lane : owner.lanes_)
                if (const auto at = lane->last_dispatch_to(root); at && (!agent || *at > *agent)) agent = at;
            signals.human_newer = human && (!agent || !agent_commanded_last(*agent, human));
        }
        signals.human_window = root && root == g_pSeatManager->m_state.keyboardFocus.lock();
        signals.agent_window = root && std::ranges::any_of(owner.lanes_, [&](const auto& lane) { return lane->drives(root); });
        signals.agent_app = agent_reaction(surface->client());
        return agent_grab(signals);
    }
    // The surface of the window a popup belongs to.
    static SP<CWLSurfaceResource> root_of(const SP<CXDGPopupResource>& popup) {
        auto parent = popup->m_parent.lock();
        while (parent && parent->m_popup) parent = parent->m_popup->m_parent.lock();
        return parent && parent->m_toplevel ? parent->m_surface.lock() : nullptr;
    }
    bool agent_reaction(wl_client* client) const {
        return std::ranges::any_of(owner.lanes_, [&](const auto& lane) { return lane->owns_reaction(client); });
    }
    // The window a new window reacts to, or null when it is not a reaction.
    PHLWINDOW anchor_of(const PHLWINDOW& window) const {
        const auto surface = window->resource();
        if (!surface) return nullptr;
        std::vector<PHLWINDOW> parents;
        // X parents come from the client's WM_TRANSIENT_FOR, which can loop.
        for (auto parent = window->parent(); parent && parents.size() < 8; parent = parent->parent()) parents.push_back(parent);
        const auto driven = [&](const PHLWINDOW& parent) {
            return std::ranges::any_of(owner.lanes_, [&](const auto& lane) { return lane->drives(parent->resource()); });
        };
        PHLWINDOW app_window;
        for (const auto& lane : owner.lanes_) {
            const auto target = lane->app_target(surface, window->m_isX11, human_commands);
            if (const auto view = target ? Desktop::View::CWLSurface::fromResource(target) : nullptr) {
                app_window = Desktop::View::CWindow::fromView(view->view());
                break;
            }
        }
        return reaction_anchor(parents, Desktop::focusState()->window(), driven, app_window);
    }
    const Impl::DrivenApp* replaced_app(const PHLWINDOW& window) const {
        for (const auto& lane : owner.lanes_)
            if (const auto app = lane->replaced_app(window, human_commands)) return app;
        return nullptr;
    }
    // The workspace of a closed driven window. Hyprland drops a workspace when
    // its last window closes, so it is made again, as a workspace rule would.
    static PHLWORKSPACE workspace_of(const Impl::DrivenApp& app) {
        if (const auto workspace = State::workspaceState()->query().id(app.workspace).run()) return workspace;
        if (app.workspace == WORKSPACE_INVALID) return nullptr;
        return State::workspaceState()->create(app.workspace, app.monitor, app.workspace_name, false);
    }
    // As Hyprland's `workspace N silent` rule does, before the window is laid
    // out, so it never shows on the human's workspace. The workspace may be
    // hidden or special.
    static void open_on(const PHLWINDOW& window, const PHLWORKSPACE& workspace) {
        if (!workspace || workspace == window->m_workspace) return;
        window->m_workspace = workspace;
        window->m_monitor = workspace->m_monitor;
        // Hyprland has already announced the window on the human's workspace.
        const auto address = reinterpret_cast<std::uintptr_t>(window.get());
        g_pEventManager->postEvent(SHyprIPCEvent{"movewindow", std::format("{:x},{}", address, workspace->m_name)});
        g_pEventManager->postEvent(SHyprIPCEvent{"movewindowv2", std::format("{:x},{},{}", address, workspace->m_id, workspace->m_name)});
    }
    // On the human's workspace the window still appears, but never over the
    // human's window, as a background app's window does on macOS.
    static void keep_below_human(const PHLWINDOW& window) {
        const auto human = Desktop::focusState()->window();
        if (!human || human == window || human->m_workspace != window->m_workspace) return;
        // Hyprland can only move a window to either end of its stack. This
        // moves the new window alone, directly under the human's.
        stack_below(const_cast<std::vector<PHLWINDOW>&>(Desktop::windowState()->windows()), window, human);
        if (Fullscreen::controller()->isFullscreen(human)) {
            // As CWindowState::lower does.
            window->m_allowedOverFullscreen = false;
            window->updateFullscreenInputState();
            *window->alpha(Desktop::View::WINDOW_ALPHA_FULLSCREEN) = window->isBlockedByFullscreen() ? 0.F : 1.F;
        }
    }
    // `live` is cleared only after both hooks are removed, so a call that
    // reaches these functions always has its original trampoline.
    static void guarded_grab(CXDGShellProtocol* self, SP<CXDGPopupResource> popup) {
        auto& guards = *live;
        const auto surface = popup && popup->m_surface ? popup->m_surface->m_surface.lock() : nullptr;
        // The popup stays mapped and still receives agent input. Only the
        // compositor-global grab, which would move human focus, is withheld.
        if (surface && guards.agent_input(surface, root_of(popup))) {
            ++guards.refused_grabs;
            guards.agent_popups.push_back(popup);
            guards.close_agent_popups(g_pSeatManager->m_state.keyboardFocus.lock());
            return;
        }
        reinterpret_cast<Grab>(guards.grab->m_original)(self, popup);
    }
    static void guarded_drag(CWLDataDeviceProtocol* self, WP<CWLDataSourceResource> source,
                             SP<CWLSurfaceResource> icon, SP<CWLSurfaceResource> origin) {
        auto& guards = *live;
        // Hyprland would drop this drag on the human's next button release.
        if (origin && guards.agent_input(origin, origin)) {
            ++guards.refused_drags;
            if (const auto s = source.lock()) s->cancelled();
            return;
        }
        reinterpret_cast<Drag>(guards.drag->m_original)(self, source, icon, origin);
    }
};

// A fullscreen or maximize request carries no seat or serial. Hyprland 0.56.2
// handles it in CWindow::onUpdateState, which its window listener inlines, so
// a function hook would never run. These listeners take the place of that one
// on every window, and call onUpdateState for each request they let through.
// Hyprland keeps a toplevel's fullscreen flag equal to its own client mode and
// sets the maximized flag on every window at map.
struct InputExperiment::StateRequests {
    static inline StateRequests* live = nullptr;
    InputExperiment& owner;
    std::uint64_t refused = 0;
    CHyprSignalListener created;

    explicit StateRequests(InputExperiment& input) : owner(input) {
        if (live) throw std::runtime_error("input state request guard already installed");
        live = this;
        // A throwing constructor never runs the destructor.
        try {
            created = Event::bus()->m_events.window.create.listen([](PHLWINDOW window) { take_over(window); });
            for (const auto& window : Desktop::windowState()->windows()) take_over(window);
        } catch (...) {
            live = nullptr;
            throw;
        }
    }
    // The listeners stay on their windows and behave as Hyprland's own once
    // `live` is cleared. The module is never unmapped (-z nodelete), so they
    // stay callable after the plugin unloads.
    ~StateRequests() { live = nullptr; }
    // Hyprland's own listener exists from the window's creation until it is
    // destroyed.
    static void take_over(const PHLWINDOW& window) {
        if (!window || !window->m_listeners.updateState) return;
        const auto request = [ref = PHLWINDOWREF(window)] {
            const auto w = ref.lock();
            if (!w || (live && w->m_isMapped && !live->admit(w))) return;
            w->onUpdateState();
            // Hyprland changes the flag only with its own fullscreen mode, and
            // would leave an app in the fullscreen a refusal gave it.
            if (const auto toplevel = toplevel_of(w); toplevel && w->m_isMapped && toplevel->m_state.requestsFullscreen.has_value())
                toplevel->setFullscreen(client_fullscreen(w));
        };
        if (const auto xdg = window->m_xdgSurface.lock(); xdg && xdg->m_toplevel)
            window->m_listeners.updateState = xdg->m_toplevel->m_events.stateChanged.listen(request);
        else if (const auto x = window->m_xwaylandSurface.lock())
            window->m_listeners.updateState = x->m_events.stateChanged.listen(request);
    }
    static SP<CXDGToplevelResource> toplevel_of(const PHLWINDOW& window) {
        const auto xdg = window->m_xdgSurface.lock();
        return xdg ? xdg->m_toplevel.lock() : nullptr;
    }
    static bool client_fullscreen(const PHLWINDOW& window) {
        return Fullscreen::controller()->getFullscreenModes(window).client == Fullscreen::FSMODE_FULLSCREEN;
    }
    bool agent(const PHLWINDOW& window) const {
        const auto surface = window->resource();
        const bool driven = std::ranges::any_of(owner.lanes_, [&](const auto& lane) { return lane->drives(surface); });
        const auto& commands = owner.reaction_guards_->human_commands;
        const bool app = std::ranges::any_of(owner.lanes_, [&](const auto& lane) { return bool(lane->app_target(surface, window->m_isX11, commands)); });
        return agent_request(window, Desktop::focusState()->window(), driven, app);
    }
    // False when the request is refused.
    bool admit(const PHLWINDOW& window) {
        const auto toplevel = toplevel_of(window);
        const auto x = window->m_xwaylandSurface.lock();
        // Minimize requests reach the same listener, and Hyprland ignores them.
        const bool requested = toplevel ? toplevel->m_state.requestsFullscreen.has_value() || toplevel->m_state.requestsMaximize.has_value() :
                                          x && (x->m_state.requestsFullscreen.has_value() || x->m_state.requestsMaximize.has_value());
        if (!requested) return true;
        const auto workspace = window->m_workspace;
        const auto fullscreen = workspace ? Fullscreen::controller()->getFullscreenWindow(workspace) : nullptr;
        switch (state_request(agent(window), window->m_pinned || (workspace && workspace->isVisible()), fullscreen && fullscreen != window)) {
        case StateRequest::apply: return true;
        case StateRequest::apply_in_place:
            // Hyprland moves a window that names an output to that output's
            // active workspace, which is the human's.
            if (toplevel) toplevel->m_state.requestsFullscreenMonitor.reset();
            return true;
        case StateRequest::refuse:
            ++refused;
            // Hyprland leaves the window as it is. Firefox waits for the
            // fullscreen it asked for and then ignores every F11, the human's
            // too, so an app still gets that flag, inside its own window. A
            // window Hyprland made fullscreen stays fullscreen. An X window
            // keeps its _NET_WM_STATE.
            if (toplevel) {
                if (toplevel->m_state.requestsFullscreen.has_value())
                    toplevel->setFullscreen(*toplevel->m_state.requestsFullscreen || client_fullscreen(window));
                toplevel->m_owner->scheduleConfigure();
            }
            return false;
        }
        return true;
    }
};

// Driver's agent cursor sits one stacking position above its target window,
// as on macOS: windows above the target cover it, and it is not drawn while
// the target is on a hidden workspace, after it closes, or after it moves or
// shrinks until Driver acts again. Driver names each cursor layer after its
// target, so these hooks only move that layer within the frame.
struct InputExperiment::CursorStacking {
    using Layer = void (*)(Render::IHyprRenderer*, PHLLS, PHLMONITOR, const Time::steady_tp&, bool, bool);
    using Window = void (*)(Render::IHyprRenderer*, PHLWINDOW, PHLMONITOR, const Time::steady_tp&, bool, Render::eRenderPassMode, bool, bool);
    static inline CursorStacking* live = nullptr;
    void* plugin;
    CFunctionHook* layer = nullptr;
    CFunctionHook* window = nullptr;
    CursorPins<PHLLSREF, PHLWINDOWREF, CBox> pins;
    CHyprSignalListener opened, destroyed;

    // The goal box, which hyprctl reports and Driver acts on.
    static CBox goal_box(const PHLWINDOW& w) { return w->geometricBox(Desktop::View::IGeometric::GEOMETRIC_GOAL); }
    // Driver stops drawing a cursor whose layer is closed until an action
    // places that cursor again, which opens a new layer.
    static void close_layer(const PHLLS& ls) {
        if (const auto resource = ls->m_layerSurface.lock()) resource->sendClosed();
    }

    explicit CursorStacking(void* handle) : plugin(handle) {
        if (live) throw std::runtime_error("cursor stacking already installed");
        live = this;
        // A throwing constructor never runs the destructor.
        try {
            layer = install_hook(plugin, "_ZN6Render13IHyprRenderer11renderLayerEN9Hyprutils6Memory14CSharedPointerIN7Desktop4View13CLayerSurfaceEEENS3_IN7Monitor8CMonitorEEERKNSt6chrono10time_pointINSB_3_V212steady_clockENSB_8durationIlSt5ratioILl1ELl1000000000EEEEEEbb",
                reinterpret_cast<void*>(render_layer));
            window = install_hook(plugin, "_ZN6Render13IHyprRenderer12renderWindowEN9Hyprutils6Memory14CSharedPointerIN7Desktop4View7CWindowEEENS3_IN7Monitor8CMonitorEEERKNSt6chrono10time_pointINSB_3_V212steady_clockENSB_8durationIlSt5ratioILl1ELl1000000000EEEEEEbNS_15eRenderPassModeEbb",
                reinterpret_cast<void*>(render_window));
            opened = Event::bus()->m_events.layer.opened.listen([this](PHLLS ls) {
                if (ls) pins.open(ls, ls->m_namespace, Desktop::windowState()->windows(), [](const PHLWINDOW& w) { return reinterpret_cast<std::uintptr_t>(w.get()); },
                    [](const PHLWINDOW& w) { return w->m_stableID; }, goal_box, close_layer);
            });
            // A window that closed on a hidden workspace never drew its cursor,
            // and releases its layer here all the same.
            destroyed = Event::bus()->m_events.window.destroy.listen([this](PHLWINDOWREF) { pins.release(close_layer); });
        } catch (...) {
            uninstall();
            throw;
        }
    }
    ~CursorStacking() { uninstall(); }
    void uninstall() {
        if (window) HyprlandAPI::removeFunctionHook(plugin, window);
        if (layer) HyprlandAPI::removeFunctionHook(plugin, layer);
        // Without the hooks Hyprland draws a pinned layer above every window.
        // Each one, bound or not yet mapped, is closed, and hidden until Driver
        // destroys it. Driver finds the input sockets gone and draws its
        // cursors in its plain layer.
        for (const auto& ls : Desktop::layerState()->layers())
            if (ls && pinned_cursor_window(ls->m_namespace)) {
                ls->alpha()[Desktop::View::LS_ALPHA_FADE]->setValueAndWarp(0.F);
                close_layer(ls);
            }
        live = nullptr;
    }
    static void render_layer(Render::IHyprRenderer* self, PHLLS ls, PHLMONITOR monitor, const Time::steady_tp& time, bool popups, bool lockscreen) {
        if (ls && pinned_cursor_window(ls->m_namespace)) return;
        reinterpret_cast<Layer>(live->layer->m_original)(self, ls, monitor, time, popups, lockscreen);
    }
    static bool has_visible_popup(const PHLWINDOW& w) {
        bool found = false;
        if (!w->m_isX11 && w->m_popupHead)
            w->m_popupHead->breadthfirst([&found](SP<Desktop::View::CPopup> popup, void*) { found = found || popup->aliveAndVisible(); }, nullptr);
        return found;
    }
    static void render_window(Render::IHyprRenderer* self, PHLWINDOW w, PHLMONITOR monitor, const Time::steady_tp& time, bool decorate,
                              Render::eRenderPassMode mode, bool ignore_position, bool standalone) {
        const auto original = reinterpret_cast<Window>(live->window->m_original);
        // Single-window captures never include the cursor, and draw the
        // window's popups as asked. Hyprland snapshots a window only to
        // animate its close, so the cursor in that snapshot fades out with
        // the window. See window_draw for the rest.
        const bool on_screen = w && monitor && !standalone && !ignore_position && w->m_isMapped && !w->isHidden();
        const auto asked = mode == Render::RENDER_PASS_ALL ? WindowPass::all : mode == Render::RENDER_PASS_MAIN ? WindowPass::main : WindowPass::popup;
        const auto draw = window_draw(asked, on_screen, on_screen && w == Desktop::focusState()->window(), on_screen && has_visible_popup(w));
        original(self, w, monitor, time, decorate, draw.first == WindowPass::main ? Render::RENDER_PASS_MAIN : mode, ignore_position, standalone);
        if (draw.cursor) draw_cursors(self, w, monitor, time);
        if (draw.popups) original(self, w, monitor, time, decorate, Render::RENDER_PASS_POPUP, ignore_position, standalone);
    }
    static void draw_cursors(Render::IHyprRenderer* self, const PHLWINDOW& w, const PHLMONITOR& monitor, const Time::steady_tp& time) {
        // Driver painted the cursor where it acted, from the box it read
        // then. It cannot see the window move, so the cursor waits for the
        // next action instead of pointing at another spot.
        live->pins.release_moved(w, goal_box(w), close_layer);
        // The cursor slides with its window when the workspace changes.
        const auto workspace = w->m_workspace;
        const auto offset = w->m_pinned || !workspace ? Vector2D{} : workspace->m_renderOffset->value();
        // Hyprland's fade alpha for the window's main pass, so the cursor
        // fades with it, a special workspace included. Opacity rules style
        // the window, not the cursor, and are left out.
        using enum Desktop::View::eWindowAlpha;
        const bool moving_out = w->m_monitorMovedFrom != -1 && (!workspace || !workspace->isVisible());
        const float fade = w->alphaValue(WINDOW_ALPHA_FADE) * w->alphaValue(WINDOW_ALPHA_FULLSCREEN) * w->alphaValue(WINDOW_ALPHA_LAYOUT) *
            (w->m_pinned || moving_out || !workspace ? 1.F : workspace->m_alpha->value()) *
            (moving_out ? w->alphaValue(WINDOW_ALPHA_MOVE_TO_WORKSPACE) : 1.F) * w->alphaValue(WINDOW_ALPHA_MOVE_FROM_WORKSPACE);
        for (const auto& ref : monitor->m_layerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY]) {
            const auto ls = ref.lock();
            if (!ls || !ls->m_mapped || !live->pins.above(ls, w)) continue;
            const auto surface = ls->wlSurface()->resource();
            if (!surface || !surface->m_current.texture) continue;
            // The layer's own open animation is skipped: the cursor appears
            // with the window it is pinned to.
            CSurfacePassElement::SRenderData data;
            data.pMonitor = monitor;
            data.when = time;
            data.pos = ls->m_geometry.pos() + offset;
            data.surface = surface;
            data.texture = surface->m_current.texture;
            data.w = ls->m_geometry.w;
            data.h = ls->m_geometry.h;
            data.pLS = ls;
            data.fadeAlpha = fade;
            data.clipBox = CBox{0, 0, monitor->m_size.x, monitor->m_size.y}.scale(monitor->m_scale).round();
            self->addPassElement(makeUnique<CSurfacePassElement>(data));
        }
    }
};

InputExperiment::InputExperiment(const std::string& directory, void* plugin) {
    SeatLifetime lifetime(directory);
    for (unsigned i = 0; i < lanes_.size(); ++i) lanes_[i] = std::make_unique<Impl>(directory, i);
    for (auto& lane : lanes_) lane->peers = {lanes_[0].get(), lanes_[1].get()};
#if defined(CUA_HYPRLAND_TEST_INPUT) || defined(CUA_HYPRLAND_INPUT_TRACE)
    trace_ = std::make_unique<PrimaryTrace>(plugin, [this](wl_resource* resource) {
        for (unsigned i = 0; i < lanes_.size(); ++i) {
            for (const auto& p : lanes_[i]->pointers) if (p->wl->resource() == resource) return i + 1;
            for (const auto& k : lanes_[i]->keyboards) if (k->wl->resource() == resource) return i + 1;
        }
        return 0u;
    });
    for (auto& lane : lanes_) lane->trace = trace_.get();
#else
    (void)plugin;
#endif
    desktop_listeners_ = std::make_unique<DesktopListeners>(*this);
#ifdef CUA_HYPRLAND_INPUT
    reaction_guards_ = std::make_unique<ReactionGuards>(*this, plugin);
    cursor_stacking_ = std::make_unique<CursorStacking>(plugin);
    state_requests_ = std::make_unique<StateRequests>(*this);
#endif
    // Seat globals come last. Clients bind a new global as soon as it is
    // announced, and destroying it after a failed hook would disconnect them.
    for (auto& lane : lanes_) lane->start();
    lifetime.publish();
}
InputExperiment::~InputExperiment() {
    state_requests_.reset();
    cursor_stacking_.reset();
    reaction_guards_.reset();
    desktop_listeners_.reset();
    for (auto& lane : lanes_) { lane->retire(); lane->trace = nullptr; lane->peers = {}; }
    trace_.reset();
    // Intentional process-lifetime ownership: callbacks, removed global, and
    // remaining client-owned resources cannot outlive their Impl. The instance
    // marker refuses replacement modules, so this retains at most two lanes.
    for (auto& lane : lanes_) (void)lane.release();
}
void InputExperiment::suspend() {
    for (auto& lane : lanes_) lane->suspend();
}
void InputExperiment::resume() {
    try {
        for (auto& lane : lanes_) lane->resume();
    } catch (...) {
        // Partial transport setup must not leave one admitted lane behind.
        suspend();
        throw;
    }
}
std::string InputExperiment::status_json() const {
    std::string states;
    for (const auto& lane : lanes_) {
        if (!states.empty()) states += ',';
        const bool pointer_focus = std::ranges::any_of(lane->pointers, [](const auto& p) { return !p->dead && bool(p->focus); });
        const bool keyboard_focus = std::ranges::any_of(lane->keyboards, [](const auto& k) { return !k->dead && bool(k->focus); });
        states += std::format(R"({{"lane":{},"epoch":"{}","desktop_generation":{},"reserved":{},"socket_cleanup":"{}","lease_active":{},"seat_resources":{},"pointer_resources":{},"keyboard_resources":{},"dispatches":{},"held_button":{},"held_keys":{},"drag_active":{},"pointer_focus":{},"keyboard_focus":{}}})",
            lane->lane, lane->epoch, lane->desktop_generation, lane->reservation != nullptr, lane->socket_cleanup, lane->lease != nullptr, lane->seats.size(), lane->pointers.size(), lane->keyboards.size(), lane->dispatches,
            lane->held_button, lane->held_keys.size(), lane->drag.has_value(), pointer_focus, keyboard_focus);
    }
    // Aggregate legacy fields remain available to existing test probes.
    return std::format(R"({{"protocol":{},"test_only":{},"seat_lifetime":"compositor","upgrade":"desktop_restart","transport_ready":{},"session_available":{},"epoch":"{}","lease_active":{},"seat_resources":{},"pointer_resources":{},"keyboard_resources":{},"dispatches":{},"primary_background":{},"refused_popup_grabs":{},"refused_drag_starts":{},"withheld_initial_focus":{},"refused_state_requests":{},"lanes":[{}]}})",
        kProduction ? 3 : 0, !kProduction, !lanes_[0]->suspended && !lanes_[1]->suspended, session_available(), lanes_[0]->epoch, lanes_[0]->lease != nullptr || lanes_[1]->lease != nullptr,
        lanes_[0]->seats.size() + lanes_[1]->seats.size(), lanes_[0]->pointers.size() + lanes_[1]->pointers.size(),
        lanes_[0]->keyboards.size() + lanes_[1]->keyboards.size(), lanes_[0]->dispatches + lanes_[1]->dispatches,
        kProduction, reaction_guards_ ? reaction_guards_->refused_grabs : 0, reaction_guards_ ? reaction_guards_->refused_drags : 0,
        reaction_guards_ ? reaction_guards_->withheld_focus : 0, state_requests_ ? state_requests_->refused : 0, states);
}
} // namespace cua::hyprland
