// agentty::tools::tool_loading — see tool_loading.hpp.

#include "agentty/tool/tool_loading.hpp"

#include <cctype>
#include <cstdlib>
#include <map>
#include <string>
#include <utility>

#include "agentty/domain/catalog.hpp"
#include "agentty/io/persistence.hpp"
#include "agentty/provider/selection.hpp"
#include "agentty/tool/subagent.hpp"
#include "agentty/util/snapshot.hpp"
#include "agentty/util/dbglog.hpp"

namespace agentty::tools::tool_loading {

namespace {

util::AtomicSnapshot<plan::Loading>& slot() {
    static util::AtomicSnapshot<plan::Loading> s;
    return s;
}

// What one search returns by default, and at most.
constexpr std::size_t kDefaultHits = 5;
constexpr std::size_t kMaxHits = 20;

ExecResult run_search(const nlohmann::json& args) {
    std::string query;
    if (args.is_object() && args.contains("query") && args["query"].is_string())
        query = args["query"].get<std::string>();
    if (query.empty())
        return std::unexpected(ToolError::invalid_args(
            "query is required: words describing the task, or select:<name>[,<name>]"));
    std::size_t k = kDefaultHits;
    if (args.is_object() && args.contains("limit") && args["limit"].is_number_integer())
        k = static_cast<std::size_t>(std::clamp<long long>(args["limit"].get<long long>(),
                                                           1, static_cast<long long>(kMaxHits)));

    const Catalog cat = wire_tools();
    std::vector<ToolDef> searchable;
    searchable.reserve(cat->size());
    for (const auto& t : *cat)
        if (t.origin != ToolOrigin::Native && t.advertise && !withheld_by_policy(t)
            && t.name.value != plan::kSearchToolName)
            searchable.push_back(t);
    const auto hits = plan::search(searchable, query, k);

    ToolOutput out;
    if (hits.empty()) {
        out.text = "No tools matched \"" + query + "\". Try other words, the server's "
                   "name, or select:<exact_name>. " + std::to_string(searchable.size())
                 + " tools are searchable.";
        return out;
    }
    // Header, then one `loaded:` line per hit. That marker is what the thread
    // reads back (plan::loaded_in) to keep these tools on the wire, and what
    // the Anthropic wire turns into tool_reference blocks. Each hit carries
    // its schema too, so a wire that can't load tools mid-turn still lets the
    // model call it right away by name.
    out.text = std::to_string(hits.size()) + (hits.size() == 1 ? " tool" : " tools")
             + " loaded for this conversation. Call them by name.\n";
    for (const auto& h : hits) {
        const auto& t = *h.tool;
        std::string desc = t.description;
        if (const auto nl = desc.find('\n'); nl != std::string::npos) desc.resize(nl);
        if (desc.size() > 200) desc = desc.substr(0, 197) + "...";
        out.text += std::string{plan::kLoadedMarker} + t.name.value + " - " + desc + "\n";
        out.text += "  schema: "
                  + (t.input_schema.is_null() ? std::string{"{}"} : t.input_schema.dump()) + "\n";
    }
    return out;
}

} // namespace

void seed(Io io) {
    if (slot().load()) return;
    plan::Loading l = plan::Loading::Auto;
    try {
        l = plan::loading_from(persistence::load_settings(io).tool_loading);
    } catch (const std::exception& e) {
        util::dbglog("tool_loading.seed", e.what());
    } catch (...) {
        util::dbglog("tool_loading.seed", "non-std exception");
    }
    (void)slot().store_if_empty(std::move(l));
}

plan::Loading current() {
    const auto s = slot().load();
    return s ? *s : plan::Loading::Auto;
}

void install(plan::Loading l) { slot().store(std::move(l)); }

const ToolDef& search_tool() {
    static const ToolDef def = [] {
        ToolDef d;
        d.name.value = std::string{plan::kSearchToolName};
        d.description =
            "Find and load more tools. Many MCP tools are not loaded yet to save "
            "context; search by what you want to do (any language, typos are "
            "fine), a server name, or select:<exact_name>[,<name>] to load known "
            "names. Loaded tools stay available for the rest of the conversation. "
            "Search before telling the user a capability is missing.";
        d.input_schema = {
            {"type", "object"},
            {"properties", {
                {"query", {{"type", "string"},
                           {"description", "What you need, e.g. \"decompile a function\", "
                                           "\"slack\", or \"select:mcp__gh__list_prs\"."}}},
                {"limit", {{"type", "integer"},
                           {"description", "How many tools to load (default 5, max 20)."}}},
            }},
            {"required", {"query"}},
        };
        d.origin = ToolOrigin::Mcp;      // not native: the planner adds it
        d.origin_id = "catalog";
        d.effects = EffectSet{};         // pure: reads the catalog only
        d.scheduling_effects = d.effects;
        d.max_output_chars = 30'000;
        d.output_truncation = OutputTruncation::Head;
        d.timeout = std::chrono::milliseconds{10'000};
        d.execute = [](const maya::unlocked&, const nlohmann::json& args,
                       const CallContext&) -> ExecResult { return run_search(args); };
        return d;
    }();
    return def;
}

plan::Limits limits_for(const provider::Selection& sel, std::size_t context_window) {
    plan::Limits l;
    l.context_window = context_window;
    if (sel.kind == provider::Kind::Anthropic) {
        // Anthropic takes deferred tools (up to 10,000) and expands
        // tool_reference blocks itself. Measured on the subscription
        // endpoint too: 124 tools, defer_loading and tool_reference all 200.
        l.discovery = plan::Discovery::DeferredReferences;
        l.max_tools = 0;
    } else {
        // OpenAI chat and Gemini reject >128 tools; OpenAI-compatible servers
        // copy that shape, and no model picks well past it anyway.
        l.discovery = plan::Discovery::ClientSide;
        l.max_tools = 128;
    }
    // AGENTTY_MCP_TOOL_BUDGET still works: it overrides the per-request cap
    // on loaded tools (0 = no cap). Unparsable or negative keeps the default.
    if (const char* e = std::getenv("AGENTTY_MCP_TOOL_BUDGET"); e && e[0]) {
        char* end = nullptr;
        const long v = std::strtol(e, &end, 10);
        if (end && *end == '\0' && v >= 0) l.max_tools = static_cast<std::size_t>(v);
    }
    return l;
}

plan::Plan plan_into(const Request& r, std::vector<provider::ToolSpec>& out) {
    // The planner works on a flat catalog. Build it from the caller's pool
    // (already filtered by policy and per-caller rules) plus tool_search.
    std::vector<ToolDef> flat;
    flat.reserve(r.pool.size());
    for (const auto* t : r.pool)
        if (t && t->name.value != plan::kSearchToolName) flat.push_back(*t);
    ToolDef search = search_tool();
    auto p = plan::make_plan(flat, r.loaded, &search, r.mode, r.limits);
    if (p.search) {
        // Say what can be found, by server, so the model knows to search
        // instead of saying a capability is missing. Sorted and stable, so
        // the bytes only change when the set of servers does.
        std::map<std::string, std::size_t> by_server;
        std::unordered_set<const ToolDef*> loaded_now;
        for (const auto& w : p.wire)
            if (w.tool && !w.deferred) loaded_now.insert(w.tool);
        for (const auto& t : flat)
            if (t.advertise && !loaded_now.contains(&t)) ++by_server[t.origin_id];
        std::string list;
        for (const auto& [srv, n] : by_server) {
            if (srv.empty()) continue;
            if (!list.empty()) list += ", ";
            list += srv + " (" + std::to_string(n) + ")";
        }
        if (!list.empty())
            search.description += " Searchable now: " + list + ".";
        for (auto& w : p.wire)
            if (w.tool && w.tool->name.value == plan::kSearchToolName) w.tool = &search;
    }
    for (const auto& w : p.wire) {
        const auto& t = *w.tool;
        out.push_back({t.name.value, t.description, t.input_schema,
                       t.eager_input_streaming, w.deferred});
    }
    // The Placement pointers point into `flat`; don't hand them back.
    for (auto& w : p.wire) w.tool = nullptr;
    return p;
}

void annotate(mcp::PluginModel& m) {
    const plan::Loading mode = current();
    m.loading = std::string{plan::to_string(mode)};

    // The model a new thread would run, for its window.
    std::size_t window = 0;
    if (const auto cfg = subagent::current(); cfg.installed && !cfg.model.empty()) {
        for (const auto& mi : cfg.candidates)
            if (mi.id.value == cfg.model && mi.context_window > 0) {
                window = static_cast<std::size_t>(mi.context_window);
                break;
            }
        if (window == 0)
            window = static_cast<std::size_t>(std::max(0, catalog_context_window_for(cfg.model)));
    }
    m.context_window = window;

    const Catalog cat = wire_tools();
    Request r;
    for (const auto& t : *cat)
        if (!withheld_by_policy(t)) r.pool.push_back(&t);
    r.limits = limits_for(provider::active(), window);
    r.mode = mode;
    std::vector<provider::ToolSpec> specs;
    const auto p = plan_into(r, specs);

    // Which (server, bare tool) pairs ride the wire. Matched through the
    // ToolDef's own origin rather than by rebuilding the wire name, which is
    // sanitised (a server called "my.srv" is mcp__my_srv__...).
    std::unordered_set<std::string> on_wire_names;
    for (const auto& s : specs)
        if (!s.defer_loading) on_wire_names.insert(s.name);
    std::unordered_set<std::string> on_wire;   // "server\0bare"
    for (const auto& t : *cat) {
        if (t.origin != ToolOrigin::Mcp || !on_wire_names.contains(t.name.value)) continue;
        const auto sep = t.name.value.rfind("__");
        const std::string bare = sep == std::string::npos ? t.name.value
                                                          : t.name.value.substr(sep + 2);
        on_wire.insert(t.origin_id + '\0' + bare);
    }
    // The wire name maps punctuation to '_' (canonical_mcp_name), so the
    // pane's raw bare name is sanitised the same way before comparing.
    auto sanitize = [](std::string s) {
        for (auto& c : s)
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-') c = '_';
        return s;
    };
    m.loaded_count = 0;
    m.on_demand_count = 0;
    for (auto& srv : m.servers) {
        if (srv.disabled || !srv.connected || srv.passthrough) continue;
        for (auto& t : srv.tools) {
            if (!t.enabled) continue;
            t.on_demand = !on_wire.contains(srv.name + '\0' + sanitize(t.name));
            (t.on_demand ? m.on_demand_count : m.loaded_count) += 1;
        }
    }
    m.schema_tokens = p.schema_tokens;
    m.capped_by_provider = p.capped_by_provider;
}

} // namespace agentty::tools::tool_loading
