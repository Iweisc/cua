#include "mock-hyprland/mock.hpp"
#include "input_experiment.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

extern "C" PLUGIN_DESCRIPTION_INFO pluginInit(HANDLE);
extern "C" void pluginExit();

namespace {
unsigned created = 0;
void check(bool value, const char* message) {
    if (!value) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}
}

// The seat owner fails as it does when a hook target is missing. Every config
// reload would otherwise announce and drop seat globals again.
namespace cua::hyprland {
struct InputExperiment::Impl {};
struct InputExperiment::DesktopListeners {};
struct InputExperiment::ReactionGuards {};
struct InputExperiment::StateRequests {};
struct InputExperiment::CursorStacking {};
class PrimaryTrace {};
InputExperiment::InputExperiment(const std::string&, void*) {
    ++created;
    throw std::runtime_error("input hook target unavailable");
}
InputExperiment::~InputExperiment() = default;
void InputExperiment::suspend() {}
void InputExperiment::resume() {}
std::string InputExperiment::status_json() const { return "{}"; }
}

int main() {
    std::array<char, 64> pattern{};
    const std::string seed = "/tmp/cua-input-failure-api-XXXXXX";
    seed.copy(pattern.data(), seed.size());
    const auto* directory = mkdtemp(pattern.data());
    check(directory != nullptr, "create isolated runtime");
    const auto hypr = std::string(directory) + "/hypr";
    const auto instance = hypr + "/mock";
    const auto socket = instance + "/cua-inject-v2.sock";
    check(mkdir(hypr.c_str(), 0700) == 0 && mkdir(instance.c_str(), 0700) == 0,
          "create private instance path");
    check(setenv("XDG_RUNTIME_DIR", directory, 1) == 0 &&
              setenv("HYPRLAND_INSTANCE_SIGNATURE", "mock", 1) == 0,
          "select runtime");
    Config::Values::configured_bool_override = false;
    static_cast<void>(pluginInit(nullptr));
    HyprlandAPI::registered_bool->set_mock_value(true);
    const std::string reported =
        R"("last_error":"input hook target unavailable; input stays off until Hyprland restarts")";
    for (unsigned reload = 0; reload < 6; ++reload) {
        Event::bus()->m_events.config.reloaded.emit();
        const auto status = HyprlandAPI::registered_legacy_command->fn(FORMAT_JSON, {});
        check(created == 1, "a failed seat owner is not retried on reload");
        check(status.find(reported) != std::string::npos && status.find(R"("ready":false)") != std::string::npos,
              "status reports why input stays off");
        check(access(socket.c_str(), F_OK) != 0, "no discovery listener is left behind");
    }
    pluginExit();
    check(Event::bus()->m_events.config.reloaded.active_count() == 0,
          "unload detaches configuration listener");
    check(rmdir(instance.c_str()) == 0 && rmdir(hypr.c_str()) == 0 && rmdir(directory) == 0,
          "remove test-owned runtime");
    std::cout << "plugin input failure tests passed\n";
}
