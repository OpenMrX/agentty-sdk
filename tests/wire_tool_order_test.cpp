// Which tools go on the wire, and in what order (tools::plan, #98).
//
// The tools block is the head of the prompt-cache prefix, so its order must
// depend only on the catalog and the thread's loaded set: never on what the
// user typed. And the web_search switch must remove only the NATIVE tool,
// with the rest keeping their order.

#include "agtest.hpp"

#include "agentty/tool/registry.hpp"
#include "agentty/tool/tool_plan.hpp"

#include <string>
#include <vector>

using agentty::ToolName;
namespace tools = agentty::tools;
namespace tp = agentty::tools::plan;

namespace {

tools::ToolDef mcp_tool(std::string name, std::string desc) {
    tools::ToolDef t;
    t.name = ToolName{std::move(name)};
    t.description = std::move(desc);
    t.origin = tools::ToolOrigin::Mcp;
    t.origin_id = "srv";
    return t;
}

tools::ToolDef native_tool(const char* n) {
    tools::ToolDef t;
    t.name = ToolName{n};
    return t;
}

// What a request carries for this catalog, with policy applied the way the
// request builders do (withheld tools never reach the planner).
std::vector<std::string> wire(const std::vector<tools::ToolDef>& cat, bool web_search_on,
                              tp::Loading mode = tp::Loading::Auto) {
    std::vector<tools::ToolDef> pool;
    for (const auto& t : cat)
        if (!tools::withheld_by_policy(t, web_search_on)) pool.push_back(t);
    const auto p = tp::make_plan(pool, {}, nullptr, mode, {});
    std::vector<std::string> out;
    for (const auto& w : p.wire) out.push_back(w.tool->name.value);
    return out;
}

}  // namespace

TEST_CASE("wire tool order is catalog order, whatever loads") {
    std::vector<tools::ToolDef> cat{native_tool("read"),
                                    mcp_tool("alpha_tickets", "jira tickets"),
                                    mcp_tool("beta_pages", "confluence pages"),
                                    mcp_tool("gamma_logs", "datadog logs")};
    const auto all = wire(cat, true);
    check(all == std::vector<std::string>{"read", "alpha_tickets", "beta_pages", "gamma_logs"},
          "natives first, then catalog order");
    // A thread that loaded the LAST tool on demand still emits catalog order.
    const auto p = tp::make_plan(cat, {"gamma_logs"}, nullptr, tp::Loading::OnDemand, {});
    std::vector<std::string> got;
    for (const auto& w : p.wire) got.push_back(w.tool->name.value);
    check(got == std::vector<std::string>{"read", "gamma_logs"}, "loaded set, catalog order");
}

// Settings → Web Search can switch web_search off. It must leave the wire
// WITHOUT disturbing the order of what remains, and only ever remove the
// NATIVE tool: an MCP server's own web_search is governed by mcp.json.
TEST_CASE("web_search switch removes only the native tool, order intact") {
    std::vector<tools::ToolDef> cat;
    for (const char* n : {"read", "web_fetch", "web_search", "grep"}) cat.push_back(native_tool(n));
    cat.push_back(mcp_tool("web_search", "a plugin's own search"));

    check(wire(cat, true) == std::vector<std::string>{"read", "web_fetch", "web_search",
                                                      "grep", "web_search"},
          "on: everything ships, catalog order");
    check(wire(cat, false) == std::vector<std::string>{"read", "web_fetch", "grep",
                                                       "web_search"},
          "off: the native web_search is gone, the rest keep their order, "
          "and the MCP tool of the same name is untouched");
}

// `agentty mcp-serve` registers native_registry() minus withheld_by_policy():
// the SAME predicate the request builders use, so another MCP client is never
// offered a tool agentty's settings switched off for its own model.
TEST_CASE("mcp-serve and the wire withhold exactly the same tools") {
    std::vector<tools::ToolDef> cat;
    for (const char* n : {"read", "web_fetch", "web_search", "grep"}) cat.push_back(native_tool(n));
    cat.push_back(mcp_tool("web_search", "a plugin's own search"));

    for (bool enabled : {true, false}) {
        std::vector<std::string> served;
        for (const auto& t : cat)
            if (!tools::withheld_by_policy(t, enabled)) served.push_back(t.name.value);
        check(served == wire(cat, enabled), "mcp-serve's list and the wire agree, on and off");
    }
    check(tools::withheld_by_policy(cat[2], false), "off withholds native web_search");
    check(!tools::withheld_by_policy(cat[2], true), "on/auto serve it");
    check(!tools::withheld_by_policy(cat[1], false), "web_fetch is never governed");
    check(!tools::withheld_by_policy(cat[4], false), "nor a plugin's tool of the same name");
}
