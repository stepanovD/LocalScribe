#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <numeric>
#include <span>
#include <vector>

namespace localscribe {

/*
 * A compact, model-free voice descriptor used as the offline fallback for
 * remote-speaker clustering. It captures the average spectral envelope and
 * its variation over voiced 25 ms frames. It is intentionally internal:
 * persisted transcript and ABI types keep only the resulting speaker ID.
 */
class SpeakerFeatureExtractor {
public:
    [[nodiscard]] static std::vector<float>
    extract(std::span<const float> samples, std::uint32_t sampleRate)
    {
        constexpr std::size_t kFftSize = 512;
        constexpr std::size_t kFilterCount = 24;
        constexpr std::size_t kCepstralCount = 12;

        if (sampleRate != 16'000 || samples.size() < 4'800) {
            return {};
        }

        constexpr std::size_t frameSamples = 400;
        constexpr std::size_t hopSamples = 160;
        std::vector<std::array<float, kCepstralCount>> cepstra;
        std::vector<float> frameEnergy;
        cepstra.reserve(samples.size() / hopSamples);
        frameEnergy.reserve(samples.size() / hopSamples);

        const auto mel = [](float frequency) {
            return 2595.0F
                * std::log10(1.0F + frequency / 700.0F);
        };
        const auto inverseMel = [](float value) {
            return 700.0F
                * (std::pow(10.0F, value / 2595.0F) - 1.0F);
        };
        const float minimumMel = mel(80.0F);
        const float maximumMel = mel(7'600.0F);
        std::array<std::size_t, kFilterCount + 2> filterBins{};
        for (std::size_t index = 0; index < filterBins.size(); ++index) {
            const float position = static_cast<float>(index)
                / static_cast<float>(filterBins.size() - 1);
            const float frequency = inverseMel(
                minimumMel + position * (maximumMel - minimumMel));
            filterBins[index] = std::min<std::size_t>(
                kFftSize / 2,
                static_cast<std::size_t>(
                    std::floor(
                        (kFftSize + 1) * frequency
                        / static_cast<float>(sampleRate))));
        }

        std::array<std::complex<float>, kFftSize> spectrum{};
        for (std::size_t offset = 0;
             offset + frameSamples <= samples.size();
             offset += hopSamples) {
            long double energy = 0.0L;
            for (std::size_t index = 0; index < frameSamples; ++index) {
                const float sample = samples[offset + index];
                energy += static_cast<long double>(sample) * sample;
            }
            const float rms = static_cast<float>(
                std::sqrt(energy / static_cast<long double>(frameSamples)));
            if (!std::isfinite(rms) || rms < 0.002F) {
                continue;
            }

            spectrum.fill({});
            float previous = offset == 0 ? 0.0F : samples[offset - 1];
            for (std::size_t index = 0; index < frameSamples; ++index) {
                const float current = samples[offset + index];
                const float emphasized = current - 0.97F * previous;
                previous = current;
                const float window = 0.54F
                    - 0.46F
                        * std::cos(
                            2.0F * std::numbers::pi_v<float>
                            * static_cast<float>(index)
                            / static_cast<float>(frameSamples - 1));
                spectrum[index] = emphasized * window;
            }
            fft(spectrum);

            std::array<float, kFftSize / 2 + 1> power{};
            for (std::size_t bin = 0; bin < power.size(); ++bin) {
                power[bin] = std::norm(spectrum[bin]);
            }

            std::array<float, kFilterCount> logBands{};
            for (std::size_t filter = 0; filter < kFilterCount; ++filter) {
                const auto left = filterBins[filter];
                const auto center = std::max(
                    filterBins[filter + 1],
                    left + 1);
                const auto right = std::max(
                    filterBins[filter + 2],
                    center + 1);
                long double sum = 0.0L;
                for (std::size_t bin = left;
                     bin < std::min(center, power.size());
                     ++bin) {
                    const float weight = static_cast<float>(bin - left)
                        / static_cast<float>(center - left);
                    sum += power[bin] * weight;
                }
                for (std::size_t bin = center;
                     bin < std::min(right, power.size());
                     ++bin) {
                    const float weight = static_cast<float>(right - bin)
                        / static_cast<float>(right - center);
                    sum += power[bin] * weight;
                }
                logBands[filter] = std::log(
                    std::max(1.0e-10F, static_cast<float>(sum)));
            }

            std::array<float, kCepstralCount> coefficients{};
            for (std::size_t coefficient = 0;
                 coefficient < kCepstralCount;
                 ++coefficient) {
                long double value = 0.0L;
                for (std::size_t band = 0; band < kFilterCount; ++band) {
                    value += logBands[band]
                        * std::cos(
                            std::numbers::pi_v<long double>
                            * static_cast<long double>(coefficient + 1)
                            * (static_cast<long double>(band) + 0.5L)
                            / static_cast<long double>(kFilterCount));
                }
                coefficients[coefficient] = static_cast<float>(value);
            }
            cepstra.push_back(coefficients);
            frameEnergy.push_back(std::log(std::max(rms, 1.0e-6F)));
        }

        /*
         * Very short Whisper segments produce unstable spectral identities
         * and are safer to attach by temporal/TDRZ continuity than to use as
         * new acoustic evidence.
         */
        if (cepstra.size() < 20) {
            return {};
        }

        std::array<long double, kCepstralCount> mean{};
        std::array<long double, kCepstralCount> variance{};
        const std::size_t trim = cepstra.size() / 10;
        for (std::size_t coefficient = 0;
             coefficient < kCepstralCount;
             ++coefficient) {
            std::vector<float> values;
            values.reserve(cepstra.size());
            for (const auto &frame : cepstra) {
                values.push_back(frame[coefficient]);
            }
            std::sort(values.begin(), values.end());
            const auto first =
                values.begin() + static_cast<std::ptrdiff_t>(trim);
            const auto last =
                values.end() - static_cast<std::ptrdiff_t>(trim);
            const std::size_t retained =
                static_cast<std::size_t>(last - first);
            for (auto value = first; value != last; ++value) {
                mean[coefficient] += *value;
            }
            mean[coefficient] /= static_cast<long double>(retained);
            for (auto value = first; value != last; ++value) {
                const long double difference =
                    *value - mean[coefficient];
                variance[coefficient] += difference * difference;
            }
            variance[coefficient] /= static_cast<long double>(retained);
        }

        std::vector<float> result;
        result.reserve(kCepstralCount * 2 + 1);
        for (const auto value : mean) {
            result.push_back(static_cast<float>(value));
        }
        for (const auto value : variance) {
            result.push_back(static_cast<float>(std::sqrt(value)));
        }

        std::sort(frameEnergy.begin(), frameEnergy.end());
        const auto energyFirst =
            frameEnergy.begin() + static_cast<std::ptrdiff_t>(trim);
        const auto energyLast =
            frameEnergy.end() - static_cast<std::ptrdiff_t>(trim);
        const std::size_t retainedEnergy =
            static_cast<std::size_t>(energyLast - energyFirst);
        const long double energyMean = std::accumulate(
            energyFirst,
            energyLast,
            0.0L)
            / static_cast<long double>(retainedEnergy);
        long double energyVariance = 0.0L;
        for (auto value = energyFirst; value != energyLast; ++value) {
            const long double difference = *value - energyMean;
            energyVariance += difference * difference;
        }
        result.push_back(static_cast<float>(
            std::sqrt(
                energyVariance
                / static_cast<long double>(retainedEnergy))));

        normalize(result);
        return result;
    }

private:
    static void fft(std::span<std::complex<float>> values)
    {
        const std::size_t count = values.size();
        for (std::size_t index = 1, reversed = 0; index < count; ++index) {
            std::size_t bit = count >> 1;
            for (; (reversed & bit) != 0; bit >>= 1) {
                reversed ^= bit;
            }
            reversed ^= bit;
            if (index < reversed) {
                std::swap(values[index], values[reversed]);
            }
        }

        for (std::size_t length = 2; length <= count; length <<= 1) {
            const float angle = -2.0F * std::numbers::pi_v<float>
                / static_cast<float>(length);
            const std::complex<float> root{
                std::cos(angle),
                std::sin(angle)};
            for (std::size_t begin = 0; begin < count; begin += length) {
                std::complex<float> factor{1.0F, 0.0F};
                for (std::size_t index = 0; index < length / 2; ++index) {
                    const auto even = values[begin + index];
                    const auto odd =
                        values[begin + index + length / 2] * factor;
                    values[begin + index] = even + odd;
                    values[begin + index + length / 2] = even - odd;
                    factor *= root;
                }
            }
        }
    }

    static void normalize(std::vector<float> &values)
    {
        long double magnitude = 0.0L;
        for (const float value : values) {
            magnitude += static_cast<long double>(value) * value;
        }
        if (magnitude <= 1.0e-12L) {
            values.clear();
            return;
        }
        const float scale =
            1.0F / static_cast<float>(std::sqrt(magnitude));
        for (float &value : values) {
            value *= scale;
        }
    }
};

} // namespace localscribe
