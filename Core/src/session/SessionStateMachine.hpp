#pragma once

#include "../common/Expected.hpp"

#include <LocalScribeCore/LocalScribeCore.h>

#include <string_view>

namespace localscribe {

class SessionStateMachine {
public:
    explicit SessionStateMachine(ls_phase_t initialPhase);

    [[nodiscard]] ls_phase_t phase() const noexcept { return phase_; }
    [[nodiscard]] Expected<void> transition(ls_phase_t next);

    [[nodiscard]] static bool isLegal(ls_phase_t from, ls_phase_t to) noexcept;
    [[nodiscard]] static bool isTerminal(ls_phase_t phase) noexcept;
    [[nodiscard]] static bool isPersisted(ls_phase_t phase) noexcept;
    [[nodiscard]] static ls_published_status_t
    publishedStatus(ls_phase_t phase) noexcept;
    [[nodiscard]] static std::string_view name(ls_phase_t phase) noexcept;

private:
    ls_phase_t phase_;
};

} // namespace localscribe
