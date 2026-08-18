#pragma once

#include "../common/Expected.hpp"
#include "../common/Types.hpp"

#include <memory>
#include <string_view>
#include <vector>

namespace localscribe {

class IAsrBackend {
public:
    virtual ~IAsrBackend() = default;
    [[nodiscard]] virtual BackendInfo info() const = 0;
    [[nodiscard]] virtual Expected<void>
    prepare(const AsrConfiguration &configuration) = 0;
    [[nodiscard]] virtual Expected<std::vector<AsrTimelineBatch>>
    accept(const AudioWindow &audio) = 0;
    [[nodiscard]] virtual Expected<std::vector<AsrTimelineBatch>> flush() = 0;
    virtual void requestAbort() noexcept {}
};

[[nodiscard]] Expected<std::unique_ptr<IAsrBackend>>
createAsrBackend(std::string_view backendId, bool allowTestBackends);

} // namespace localscribe
