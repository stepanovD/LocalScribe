#pragma once

#include "AsrBackend.hpp"

#include <memory>

namespace localscribe {

class WhisperCppBackend final : public IAsrBackend {
public:
    WhisperCppBackend();
    ~WhisperCppBackend() override;
    WhisperCppBackend(const WhisperCppBackend &) = delete;
    WhisperCppBackend &operator=(const WhisperCppBackend &) = delete;

    [[nodiscard]] BackendInfo info() const override;
    [[nodiscard]] Expected<void>
    prepare(const AsrConfiguration &configuration) override;
    [[nodiscard]] Expected<std::vector<AsrTimelineBatch>>
    accept(const AudioWindow &audio) override;
    [[nodiscard]] Expected<std::vector<AsrTimelineBatch>> flush() override;
    void requestAbort() noexcept override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace localscribe
