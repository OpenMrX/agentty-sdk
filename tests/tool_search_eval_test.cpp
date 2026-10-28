// tool_search_eval_test — #98: does tool_search find the right tool?
//
// A realistic catalog (the reporter's Binary Ninja server plus GitHub, Slack,
// Postgres, filesystem, browser and Jira servers; ~130 tools with the names
// and descriptions those servers really ship) and queries the way people
// write them: plain task words, the server's own jargon, typos, other word
// forms, and the query in German, Spanish and French with English tool text.
//
// Floors are set below what the search scores today, so a regression fails
// and an improvement can raise them. hit@5 matters most: tool_search loads
// up to 5, so a tool in the top 5 is a tool the model can call.

#include "agtest.hpp"

#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "agentty/tool/tool_plan.hpp"

namespace tp = agentty::tools::plan;
using agentty::tools::ToolDef;
using agentty::tools::ToolOrigin;

namespace {

ToolDef t(const std::string& srv, const std::string& name, const std::string& desc,
          std::vector<std::string> args = {}) {
    ToolDef d;
    d.name.value = "mcp__" + srv + "__" + name;
    d.description = "[MCP " + srv + "] " + desc;
    d.origin = ToolOrigin::Mcp;
    d.origin_id = srv;
    nlohmann::json props = nlohmann::json::object();
    for (auto& a : args) props[a] = {{"type", "string"}};
    d.input_schema = {{"type", "object"}, {"properties", props}};
    return d;
}

std::vector<ToolDef> catalog() {
    std::vector<ToolDef> c;
    // Binary Ninja (the #98 reporter's server; names from binary_ninja_mcp).
    const char* bn[][3] = {
        {"decompile_function", "Decompile a function to high level IL pseudo C", "name"},
        {"get_il", "Get the IL (LLIL, MLIL, HLIL) of a function", "name"},
        {"fetch_disassembly", "Get the assembly disassembly of a function", "name"},
        {"rename_function", "Rename a function", "old_name"},
        {"rename_single_variable", "Rename a local variable in a function", "variable_name"},
        {"set_comment", "Set a comment at an address", "address"},
        {"set_function_comment", "Set a comment on a function", "function_name"},
        {"get_xrefs_to", "Get cross references (xrefs) to an address", "address"},
        {"get_xrefs_to_field", "Get cross references to a struct field", "struct_name"},
        {"list_strings", "List strings in the binary", "offset"},
        {"list_imports", "List imported functions and symbols", "offset"},
        {"list_exports", "List exported functions and symbols", "offset"},
        {"list_segments", "List memory segments of the binary", "offset"},
        {"list_sections", "List sections of the binary", "offset"},
        {"list_methods", "List all function names in the program", "offset"},
        {"list_classes", "List all namespace and class names", "offset"},
        {"search_functions_by_name", "Search for functions whose name contains a substring", "query"},
        {"get_entry_points", "List the entry points of the binary", ""},
        {"define_types", "Define C types from a declaration", "c_code"},
        {"declare_c_type", "Create or update a local type from a C declaration", "c_declaration"},
        {"retype_variable", "Change the type of a local variable", "variable_name"},
        {"set_function_prototype", "Set the prototype signature of a function", "prototype"},
        {"make_function_at", "Create a function at an address", "address"},
        {"patch_bytes", "Write bytes at an address", "address"},
        {"get_binary_status", "Get the currently loaded binary and analysis state", ""},
        {"hexdump_address", "Hexdump memory at an address", "address"},
        {"get_data_decl", "Get the declaration of a data variable", "name"},
        {"function_at", "Find the function containing an address", "address"},
        {"get_stack_frame_vars", "Get stack frame variables of a function", "function_identifier"},
        {"list_binaries", "List the open binaries", ""},
        {"select_binary", "Select which open binary to work on", "view"},
        {"format_value", "Convert and annotate a value at an address", "address"},
    };
    for (auto& r : bn) c.push_back(t("binary_ninja", r[0], r[1], {r[2]}));
    // GitHub (names from github-mcp-server).
    const char* gh[][2] = {
        {"create_issue", "Create a new issue in a GitHub repository"},
        {"get_issue", "Get details of a specific issue"},
        {"list_issues", "List issues in a repository"},
        {"update_issue", "Update an existing issue"},
        {"add_issue_comment", "Add a comment to an issue"},
        {"search_issues", "Search for issues and pull requests"},
        {"create_pull_request", "Create a new pull request"},
        {"get_pull_request", "Get details of a pull request"},
        {"list_pull_requests", "List pull requests in a repository"},
        {"merge_pull_request", "Merge a pull request"},
        {"get_pull_request_diff", "Get the diff of a pull request"},
        {"create_branch", "Create a new branch"},
        {"list_branches", "List branches in a repository"},
        {"get_file_contents", "Get the contents of a file or directory from a repository"},
        {"create_or_update_file", "Create or update a single file in a repository"},
        {"push_files", "Push multiple files in a single commit"},
        {"list_commits", "Get the list of commits of a branch"},
        {"search_code", "Search for code across GitHub repositories"},
        {"search_repositories", "Search for GitHub repositories"},
        {"fork_repository", "Fork a repository to your account"},
        {"create_repository", "Create a new repository"},
        {"list_workflow_runs", "List GitHub Actions workflow runs"},
        {"get_job_logs", "Get logs for a GitHub Actions workflow job"},
        {"rerun_workflow_run", "Re-run a workflow run"},
        {"list_releases", "List releases of a repository"},
        {"create_release", "Create a release"},
        {"list_notifications", "List notifications for the authenticated user"},
        {"request_copilot_review", "Request a Copilot code review for a pull request"},
    };
    for (auto& r : gh) c.push_back(t("github", r[0], r[1], {"owner", "repo"}));
    // Slack.
    const char* sl[][2] = {
        {"slack_list_channels", "List public channels in the workspace"},
        {"slack_post_message", "Post a new message to a Slack channel"},
        {"slack_reply_to_thread", "Reply to a specific message thread"},
        {"slack_add_reaction", "Add an emoji reaction to a message"},
        {"slack_get_channel_history", "Get recent messages from a channel"},
        {"slack_get_thread_replies", "Get all replies in a message thread"},
        {"slack_get_users", "Get a list of all users in the workspace"},
        {"slack_get_user_profile", "Get detailed profile information for a user"},
        {"slack_search_messages", "Search messages across channels"},
    };
    for (auto& r : sl) c.push_back(t("slack", r[0], r[1], {"channel_id"}));
    // Postgres.
    const char* pg[][2] = {
        {"query", "Run a read-only SQL query"},
        {"list_schemas", "List database schemas"},
        {"list_tables", "List tables in a schema"},
        {"describe_table", "Show the columns, types and indexes of a table"},
        {"explain_query", "Explain the execution plan of a SQL query"},
        {"get_top_queries", "Report the slowest queries by total time"},
        {"analyze_db_health", "Check index, vacuum and connection health"},
        {"list_extensions", "List installed extensions"},
    };
    for (auto& r : pg) c.push_back(t("postgres", r[0], r[1], {"sql"}));
    // Browser automation (playwright-mcp names).
    const char* br[][2] = {
        {"browser_navigate", "Navigate to a URL"},
        {"browser_click", "Click an element on the page"},
        {"browser_type", "Type text into an editable element"},
        {"browser_take_screenshot", "Take a screenshot of the current page"},
        {"browser_snapshot", "Capture an accessibility snapshot of the page"},
        {"browser_fill_form", "Fill multiple form fields"},
        {"browser_select_option", "Select an option in a dropdown"},
        {"browser_press_key", "Press a key on the keyboard"},
        {"browser_wait_for", "Wait for text to appear or a time to pass"},
        {"browser_tabs", "List, create, close or select a browser tab"},
        {"browser_console_messages", "Return all console messages"},
        {"browser_network_requests", "Return all network requests since loading the page"},
        {"browser_file_upload", "Upload one or more files"},
        {"browser_evaluate", "Evaluate a JavaScript expression on the page"},
        {"browser_close", "Close the page"},
    };
    for (auto& r : br) c.push_back(t("playwright", r[0], r[1], {"element"}));
    // Jira.
    const char* ji[][2] = {
        {"jira_get_issue", "Get details of a Jira issue"},
        {"jira_search", "Search Jira issues using JQL"},
        {"jira_create_issue", "Create a new Jira issue"},
        {"jira_update_issue", "Update an existing Jira issue"},
        {"jira_transition_issue", "Move an issue to a new status"},
        {"jira_add_comment", "Add a comment to a Jira issue"},
        {"jira_get_sprints_from_board", "Get sprints of a board"},
        {"jira_add_worklog", "Log work time on an issue"},
        {"jira_link_to_epic", "Link an issue to an epic"},
    };
    for (auto& r : ji) c.push_back(t("jira", r[0], r[1], {"issue_key"}));
    // Sentry + Grafana, so names like "issue" and "query" are contested.
    const char* se[][3] = {
        {"sentry", "get_sentry_issue", "Get a Sentry error issue with its stack trace"},
        {"sentry", "list_sentry_issues", "List unresolved Sentry issues for a project"},
        {"sentry", "search_events", "Search Sentry error events"},
        {"grafana", "query_prometheus", "Run a PromQL query against Prometheus"},
        {"grafana", "query_loki_logs", "Query logs with LogQL from Loki"},
        {"grafana", "list_dashboards", "List Grafana dashboards"},
        {"grafana", "get_dashboard", "Get a dashboard by UID"},
        {"grafana", "list_alert_rules", "List alert rules"},
    };
    for (auto& r : se) c.push_back(t(r[0], r[1], r[2], {"id"}));
    return c;
}

struct Q { const char* query; const char* want; const char* kind; };

const Q kQueries[] = {
    // Task words, English.
    {"decompile this function", "decompile_function", "task"},
    {"show me the pseudo C for main", "decompile_function", "task"},
    {"who calls this address", "get_xrefs_to", "task"},
    {"rename the function", "rename_function", "task"},
    {"rename a local variable", "rename_single_variable", "task"},
    {"list the strings in the binary", "list_strings", "task"},
    {"what does this binary import", "list_imports", "task"},
    {"change a variable's type", "retype_variable", "task"},
    {"open a pull request", "create_pull_request", "task"},
    {"merge the PR", "merge_pull_request", "task"},
    {"file a bug on github", "create_issue", "task"},
    {"why did CI fail, get the job logs", "get_job_logs", "task"},
    {"send a message to the team channel", "slack_post_message", "task"},
    {"react with an emoji", "slack_add_reaction", "task"},
    {"run a SQL query", "query", "task"},
    {"what columns does the users table have", "describe_table", "task"},
    {"which queries are slow", "get_top_queries", "task"},
    {"take a screenshot of the page", "browser_take_screenshot", "task"},
    {"go to this url", "browser_navigate", "task"},
    {"click the login button", "browser_click", "task"},
    {"move the ticket to done", "jira_transition_issue", "task"},
    {"log 2 hours on the ticket", "jira_add_worklog", "task"},
    {"show the stack trace of the crash", "get_sentry_issue", "task"},
    {"search the logs", "query_loki_logs", "task"},
    // Jargon and identifiers.
    {"xrefs", "get_xrefs_to", "jargon"},
    {"hlil", "get_il", "jargon"},
    {"hexdump", "hexdump_address", "jargon"},
    {"JQL", "jira_search", "jargon"},
    {"PromQL", "query_prometheus", "jargon"},
    {"set_function_prototype", "set_function_prototype", "jargon"},
    {"get_pull_request_diff", "get_pull_request_diff", "jargon"},
    // Typos and word forms.
    {"decomple functon", "decompile_function", "typo"},
    {"renaming varaibles", "rename_single_variable", "typo"},
    {"screenshoot", "browser_take_screenshot", "typo"},
    {"crete pull reqest", "create_pull_request", "typo"},
    {"imported symbols", "list_imports", "typo"},
    {"comments on functions", "set_function_comment", "typo"},
    // Other languages, English tool text (the reporter's concern).
    {"Funktion dekompilieren decompile", "decompile_function", "lang"},
    {"Pull Request erstellen", "create_pull_request", "lang"},
    {"captura de pantalla screenshot", "browser_take_screenshot", "lang"},
    {"renommer la fonction rename", "rename_function", "lang"},
    {"Kommentar zu einer Funktion setzen comment", "set_function_comment", "lang"},
};

} // namespace

TEST_CASE("tool search eval: the right tool is in what tool_search loads (#98)") {
    const auto cat = catalog();
    int n = 0, hit1 = 0, hit5 = 0;
    struct Bucket { int n = 0, h1 = 0, h5 = 0; };
    std::map<std::string, Bucket> by_kind;
    for (const auto& q : kQueries) {
        const auto hits = tp::search(cat, q.query, 5);
        int rank = -1;
        for (std::size_t i = 0; i < hits.size(); ++i) {
            const auto& name = hits[i].tool->name.value;
            if (name.ends_with(std::string{"__"} + q.want)) { rank = static_cast<int>(i); break; }
        }
        ++n;
        auto& b = by_kind[q.kind];
        ++b.n;
        if (rank == 0) { ++hit1; ++b.h1; }
        if (rank >= 0) { ++hit5; ++b.h5; }
        if (rank != 0)
            std::printf("  %-44s want %-26s got %s (rank %d)\n", q.query, q.want,
                        hits.empty() ? "-" : hits[0].tool->name.value.c_str(), rank);
    }
    std::printf("tool search eval: %zu tools, %d queries, hit@1 %.2f, hit@5 %.2f\n",
                cat.size(), n, static_cast<double>(hit1) / n, static_cast<double>(hit5) / n);
    for (const auto& [k, b] : by_kind)
        std::printf("  %-7s n=%-3d hit@1 %.2f hit@5 %.2f\n", k.c_str(), b.n,
                    static_cast<double>(b.h1) / b.n, static_cast<double>(b.h5) / b.n);
    CHECK(static_cast<double>(hit5) / n >= 0.95);
    CHECK(static_cast<double>(hit1) / n >= 0.85);
}
