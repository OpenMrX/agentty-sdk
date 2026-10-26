#pragma once
// agentty::util::Callback — a published hook that is only ever invoked.
//
// A subsystem publishes a settings observer, a list-changed hook, a teardown
// action once, and other threads only call it. A plain std::function isn't
// Frozen (the closure may reach anything), so maya::published<const
// std::function<..>> is refused. This names the one case where that is fine:
// invoked, never reassigned, and what the closure reaches is its owner's to
// synchronise. One opt-in here instead of one per call site, and a grep for
// Callback finds every such hook.

#include <functional>
#include <type_traits>
#include <utility>

#include <maya/runtime.hpp>

namespace agentty::util {

template <class Sig>
struct Callback {
    std::function<Sig> fn;

    Callback() = default;
    // Implicit, like std::function's: a Callback drops in where one was.
    template <class F>
        requires(!std::is_same_v<std::remove_cvref_t<F>, Callback>
                 && std::is_constructible_v<std::function<Sig>, F>)
    Callback(F&& f) : fn(std::forward<F>(f)) {}

    template <class... A>
    decltype(auto) operator()(A&&... a) const { return fn(std::forward<A>(a)...); }
    explicit operator bool() const noexcept { return static_cast<bool>(fn); }
};

} // namespace agentty::util

template <class Sig> MAYA_SENDABLE_T(agentty::util::Callback<Sig>);
template <class Sig> MAYA_FROZEN_T(agentty::util::Callback<Sig>);
