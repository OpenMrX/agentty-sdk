// settings_default_test — pins the SHIPPED default permission profile.
//
// A fresh install (no settings.json) and a legacy/partial config missing the
// "profile" key must BOTH resolve to Write — the Profile enum's zero value.
// This is an intentional product decision: agentty is autonomous-by-default,
// contained by the sandbox + workspace boundary (the website documents it at
// /docs/profiles, and the ACP bridge separately starts in Ask). This test is
// the regression lock so a later enum reorder or a Settings-struct-default
// change can't silently flip the default and desync the docs.
//
// It drives the PUBLIC persistence API (load_settings / save_settings)
// against an isolated $HOME so it touches real disk, not a mock.

#include "agentty/io/persistence.hpp"
#include "agentty/store/store.hpp"
#include "agentty/domain/profile.hpp"
#include "agentty/domain/sandbox_config.hpp"
#include "agentty/runtime/panel/settings/items.hpp"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "agtest.hpp"

#include <unistd.h>   // getpid

namespace fs = std::filesystem;
using namespace agentty;

// Compile-time twin of the runtime checks below (also enforced project-wide in
// domain/profile.hpp): Write must stay the enum's zero value, because
// load_settings() defaults a missing "profile" key to value("profile", 0).
static_assert(static_cast<std::uint8_t>(Profile::Write) == 0,
              "Profile::Write must remain the enum's zero value — the settings "
              "loader defaults a missing 'profile' key to 0, which must == Write.");

TEST_CASE("settings defaults + persistence round-trip") {
    // Isolate the data dir: persistence::data_dir() resolves $HOME/.agentty.
    auto tmp = fs::temp_directory_path()
             / ("agentty_settings_test_" + std::to_string(::getpid()));
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    ::setenv("HOME", tmp.c_str(), 1);
    // Settings/user-scope paths resolve via util::user_root()
    // ($AGENTTY_HOME, else $HOME/.agentty) — keep both in the sandbox.
    ::unsetenv("AGENTTY_HOME");   // fall back to $HOME/.agentty
    ::unsetenv("USERPROFILE");   // data_dir() prefers this on Windows; clear it

    const auto settings_json = tmp / ".agentty" / "settings.json";

    // (1) The struct default — the value a default-constructed Settings holds.
    check(store::Settings{}.profile == Profile::Write,
          "Settings{}.profile defaults to Write");

    // (2) Fresh install: nothing persisted yet → load falls back to Write.
    check(!fs::exists(settings_json), "no settings.json present (fresh install)");
    check(persistence::load_settings(::agentty::IoAccess::grant()).profile == Profile::Write,
          "load_settings() with no file -> Write");

    // (3) Legacy/partial config that predates the "profile" key → Write.
    fs::create_directories(tmp / ".agentty");
    {
        std::ofstream ofs(settings_json, std::ios::trunc);
        ofs << R"({"model_id":"claude-x","favorite_models":[]})";
    }
    check(persistence::load_settings(::agentty::IoAccess::grant()).profile == Profile::Write,
          "load_settings() with settings.json missing 'profile' -> Write");

    // (4) A non-default choice still round-trips — proves the Write default
    //     isn't masking a broken parser (Ask/Minimal persist and reload).
    {
        store::Settings s; s.profile = Profile::Ask;
        persistence::save_settings(::agentty::IoAccess::grant(), agtest::cap(), s);
    }
    check(persistence::load_settings(::agentty::IoAccess::grant()).profile == Profile::Ask,
          "save/load round-trips a non-default profile (Ask)");
    {
        store::Settings s; s.profile = Profile::Minimal;
        persistence::save_settings(::agentty::IoAccess::grant(), agtest::cap(), s);
    }
    check(persistence::load_settings(::agentty::IoAccess::grant()).profile == Profile::Minimal,
          "save/load round-trips Minimal");

    // (5) Sandbox: an emptied port list stays empty.
    {
        store::Settings s;
        s.sandbox.configured = true;
        s.sandbox.allow_ports.clear();
        persistence::save_settings(::agentty::IoAccess::grant(), agtest::cap(), s);
    }
    check(persistence::load_settings(::agentty::IoAccess::grant()).sandbox.allow_ports.empty(),
          "an empty allow_ports list round-trips as empty");

    // (6) Bad sandbox values keep the default instead of breaking the load.
    {
        std::ofstream ofs(settings_json, std::ios::trunc);
        ofs << R"({"profile":1,"sandbox":{"configured":true,"net_mode":9,)"
               R"("syscall_mode":-1,"memory_mb":"lots","wx_protect":"yes"},)"
               R"("rag":{"configured":true}})";
    }
    {
        const auto s = persistence::load_settings(::agentty::IoAccess::grant());
        const sandbox_cfg::Config def;
        check(s.sandbox.net_mode == def.net_mode, "out-of-range net_mode keeps the default");
        check(s.sandbox.syscall_mode == def.syscall_mode, "negative syscall_mode keeps the default");
        check(s.sandbox.memory_mb == def.memory_mb, "string memory_mb keeps the default");
        check(s.sandbox.wx_protect == def.wx_protect, "string wx_protect keeps the default");
        check(s.rag.configured, "a bad sandbox value doesn't drop later sections");
    }

    fs::remove_all(tmp);
}

TEST_CASE("sandbox user paths resolve to absolute") {
    using sandbox_cfg::resolve_user_path;
    check(resolve_user_path("~/.cargo", "/w", "/home/u") == "/home/u/.cargo", "~/x expands");
    check(resolve_user_path("~", "/w", "/home/u") == "/home/u", "~ expands");
    check(resolve_user_path("build/out/", "/w", "/home/u") == "/w/build/out",
          "relative resolves against the workspace, trailing / dropped");
    check(resolve_user_path("/opt/x/../y", "/w", "/home/u") == "/opt/y", "absolute is normalised");
    check(resolve_user_path("~/x", "/w", "").empty(), "~ with no home is dropped");
    check(resolve_user_path("/", "/w", "/home/u") == "/", "/ stays /");
}

TEST_CASE("settings rows harden bad input") {
    namespace S = agentty::settings;
    auto tmp = fs::temp_directory_path()
             / ("agentty_settings_harden_" + std::to_string(::getpid()));
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    ::setenv("HOME", tmp.c_str(), 1);
    // Settings/user-scope paths resolve via util::user_root()
    // ($AGENTTY_HOME, else $HOME/.agentty) — keep both in the sandbox.
    ::unsetenv("AGENTTY_HOME");   // fall back to $HOME/.agentty
    ::unsetenv("USERPROFILE");

    // create_starter: a ':'-namespaced command becomes a SUBDIRECTORY tree
    // (git:fixup -> commands/git/fixup.md), matching the loader and never
    // writing a ':' into a filename (illegal on Windows).
    {
        auto r = S::create_starter(::agentty::IoAccess::grant(), S::Category::Commands, "git:fixup");
        check(r.ok, "create_starter nests a colon name");
        check(fs::is_regular_file(tmp / ".agentty" / "commands" / "git" / "fixup.md"),
              "colon name written as git/fixup.md subdir tree");
        check(!fs::exists(tmp / ".agentty" / "commands" / "git:fixup.md"),
              "no literal-colon filename is created");
    }
    // A plain name is a single file.
    {
        auto r = S::create_starter(::agentty::IoAccess::grant(), S::Category::Commands, "deploy");
        check(r.ok && fs::is_regular_file(tmp / ".agentty" / "commands" / "deploy.md"),
              "plain name -> deploy.md");
    }
    // Duplicate is refused, not silently overwritten.
    check(!S::create_starter(::agentty::IoAccess::grant(), S::Category::Commands, "deploy").ok,
          "create_starter refuses an existing file");
    // Path-escape and illegal shapes are rejected up front.
    check(!S::create_starter(::agentty::IoAccess::grant(), S::Category::Commands, "../evil").ok,
          "'..' path escape rejected");
    check(!S::create_starter(::agentty::IoAccess::grant(), S::Category::Commands, "a/b").ok,
          "raw slash rejected");
    check(!S::create_starter(::agentty::IoAccess::grant(), S::Category::Commands, ".hidden").ok,
          "leading-dot segment rejected");
    check(!S::create_starter(::agentty::IoAccess::grant(), S::Category::Commands, "").ok,
          "empty name rejected");
    check(!S::create_starter(::agentty::IoAccess::grant(), S::Category::Commands, std::string(200, 'x')).ok,
          "over-long name rejected");
    check(!S::create_starter(::agentty::IoAccess::grant(), S::Category::Commands, "a:b:c:d").ok,
          "too many nesting levels rejected");

    // add_plugin_from_line: a misplaced flag as the name is rejected, a
    // non-existent --python script is rejected, and a good spec is accepted.
    check(!S::add_plugin_from_line(::agentty::IoAccess::grant(), agtest::cap(), "--http http://x").ok,
          "leading-dash name (misplaced flag) rejected");
    check(!S::add_plugin_from_line(::agentty::IoAccess::grant(), agtest::cap(), "tool --python /no/such/script_xyz.py").ok,
          "non-existent --python script rejected");
    {
        auto script = tmp / "srv.py";
        std::ofstream(script) << "print('hi')\n";
        auto r = S::add_plugin_from_line(::agentty::IoAccess::grant(), agtest::cap(), "mysrv --python " + script.string());
        check(r.ok, "valid --python plugin accepted");
    }

    fs::remove_all(tmp);
}
