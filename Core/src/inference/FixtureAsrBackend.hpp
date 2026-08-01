#pragma once

#include "AsrBackend.hpp"

namespace localscribe {

class FixtureAsrBackend final : public IAsrBackend {
public:
    [[nodiscard]] BackendInfo info() const override;
    [[nodiscard]] Expected<void>
    prepare(const AsrConfiguration &configuration) override;
    [[nodiscard]] Expected<std::vector<AsrHypothesis>>
    accept(const AudioWindow &audio) override;
    [[nodiscard]] Expected<std::vector<AsrHypothesis>> flush() override;

private:
    AsrConfiguration configuration_;
    bool prepared_{false};
};

} // namespace localscribe
