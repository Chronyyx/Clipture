#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace clipture {

constexpr std::size_t boundedWriteSize(std::size_t remaining, std::size_t maximum) {
    return maximum == 0 ? 0 : std::min(remaining, maximum);
}

enum class StorageSeekPenalty {
    Unknown,
    DoesNotIncur,
    Incurs
};

constexpr bool shouldUseAdaptiveWritePacing(
    bool storageAware,
    StorageSeekPenalty /*seekPenalty*/) {
    // Cached writes can outrun even solid-state storage and leave a large dirty
    // cache to drain at close. Storage class selects the rate profile below;
    // every physical output still benefits from a bounded sustained rate.
    return storageAware;
}

enum class AdaptiveWritePressure {
    Healthy,
    Elevated,
    Critical
};

struct AdaptiveWritePacerConfig {
    uint64_t initialBytesPerSecond = 96ULL * 1024ULL * 1024ULL;
    uint64_t minimumBytesPerSecond = 16ULL * 1024ULL * 1024ULL;
    uint64_t maximumLearnedBytesPerSecond = 4ULL * 1024ULL * 1024ULL * 1024ULL;
    std::size_t adjustmentWindowBytes = 16u * 1024u * 1024u;
    uint32_t targetUtilizationPercent = 75;
    uint32_t minimumMeasuredServicePercent = 20;
    uint64_t minimumMeasuredWriteUs = 250;
};

constexpr AdaptiveWritePacerConfig writePacerConfigForStorage(
    AdaptiveWritePacerConfig config,
    StorageSeekPenalty seekPenalty) {
    if (seekPenalty != StorageSeekPenalty::DoesNotIncur) return config;

    constexpr uint64_t mib = 1024ULL * 1024ULL;
    constexpr uint64_t solidStateInitialRate = 640ULL * mib;
    constexpr uint64_t solidStateMaximumRate = 768ULL * mib;
    constexpr std::size_t solidStateAdjustmentWindow = 128u * 1024u * 1024u;

    config.maximumLearnedBytesPerSecond = std::min(
        config.maximumLearnedBytesPerSecond,
        solidStateMaximumRate);
    config.initialBytesPerSecond = std::min(
        std::max(config.initialBytesPerSecond, solidStateInitialRate),
        config.maximumLearnedBytesPerSecond);
    config.adjustmentWindowBytes = std::max(
        config.adjustmentWindowBytes,
        solidStateAdjustmentWindow);
    return config;
}

struct SustainedWritePressureConfig {
    uint64_t elevatedDelayUs = 50'000;
    uint64_t healthyRecoveryUs = 250'000;
};

class SustainedWritePressureGate {
public:
    explicit SustainedWritePressureGate(SustainedWritePressureConfig config = {})
        : config_(config) {}

    AdaptiveWritePressure update(AdaptiveWritePressure pressure, uint64_t nowUs) {
        if (pressure == AdaptiveWritePressure::Critical) {
            elevatedSinceUs_ = unsetTime;
            healthySinceUs_ = unsetTime;
            effectivePressure_ = AdaptiveWritePressure::Critical;
            return effectivePressure_;
        }

        if (pressure == AdaptiveWritePressure::Elevated) {
            healthySinceUs_ = unsetTime;
            if (effectivePressure_ == AdaptiveWritePressure::Critical) {
                effectivePressure_ = AdaptiveWritePressure::Elevated;
                elevatedSinceUs_ = unsetTime;
                return effectivePressure_;
            }
            if (effectivePressure_ == AdaptiveWritePressure::Healthy) {
                if (elevatedSinceUs_ == unsetTime) {
                    elevatedSinceUs_ = nowUs;
                } else if (nowUs - elevatedSinceUs_ >= config_.elevatedDelayUs) {
                    effectivePressure_ = AdaptiveWritePressure::Elevated;
                    elevatedSinceUs_ = unsetTime;
                }
            }
            return effectivePressure_;
        }

        elevatedSinceUs_ = unsetTime;
        if (effectivePressure_ == AdaptiveWritePressure::Healthy) {
            healthySinceUs_ = unsetTime;
            return effectivePressure_;
        }
        if (healthySinceUs_ == unsetTime) {
            healthySinceUs_ = nowUs;
        } else if (nowUs - healthySinceUs_ >= config_.healthyRecoveryUs) {
            effectivePressure_ = AdaptiveWritePressure::Healthy;
            healthySinceUs_ = unsetTime;
        }
        return effectivePressure_;
    }

    AdaptiveWritePressure effectivePressure() const { return effectivePressure_; }

private:
    static constexpr uint64_t unsetTime = UINT64_MAX;

    SustainedWritePressureConfig config_;
    AdaptiveWritePressure effectivePressure_ = AdaptiveWritePressure::Healthy;
    uint64_t elevatedSinceUs_ = unsetTime;
    uint64_t healthySinceUs_ = unsetTime;
};

class AdaptiveWriteRateController {
public:
    explicit AdaptiveWriteRateController(AdaptiveWritePacerConfig config = {})
        : config_(config) {
        config_.minimumBytesPerSecond = std::max<uint64_t>(config_.minimumBytesPerSecond, 1);
        config_.maximumLearnedBytesPerSecond = std::max(
            config_.maximumLearnedBytesPerSecond,
            config_.minimumBytesPerSecond);
        config_.initialBytesPerSecond = std::clamp(
            config_.initialBytesPerSecond,
            config_.minimumBytesPerSecond,
            config_.maximumLearnedBytesPerSecond);
        config_.adjustmentWindowBytes = std::max<std::size_t>(config_.adjustmentWindowBytes, 1);
        config_.targetUtilizationPercent = std::clamp<uint32_t>(
            config_.targetUtilizationPercent,
            1,
            95);
        config_.minimumMeasuredServicePercent = std::clamp<uint32_t>(
            config_.minimumMeasuredServicePercent,
            1,
            config_.targetUtilizationPercent);
        config_.minimumMeasuredWriteUs = std::max<uint64_t>(config_.minimumMeasuredWriteUs, 1);

        currentBytesPerSecond_ = config_.initialBytesPerSecond;
        minimumRateSeen_ = currentBytesPerSecond_;
        maximumRateSeen_ = currentBytesPerSecond_;
    }

    void observePressure(AdaptiveWritePressure pressure) {
        if (pressure == pressure_) return;

        const auto previousPressure = pressure_;
        pressure_ = pressure;
        healthyBytes_ = 0;

        if (pressure == AdaptiveWritePressure::Critical) {
            setRate(std::max(dynamicMinimumBytesPerSecond(), currentBytesPerSecond_ / 2));
            ++pressureBackoffs_;
        } else if (
            pressure == AdaptiveWritePressure::Elevated &&
            previousPressure == AdaptiveWritePressure::Healthy) {
            setRate(std::max(
                dynamicMinimumBytesPerSecond(),
                currentBytesPerSecond_ - currentBytesPerSecond_ / 4));
            ++pressureBackoffs_;
        } else if (
            pressure == AdaptiveWritePressure::Healthy &&
            previousPressure != AdaptiveWritePressure::Healthy &&
            observedServiceBytesPerSecond_ > 0) {
            const uint64_t healthyTarget = std::clamp(
                observedServiceBytesPerSecond_ * config_.targetUtilizationPercent / 100,
                dynamicMinimumBytesPerSecond(),
                config_.maximumLearnedBytesPerSecond);
            if (healthyTarget > currentBytesPerSecond_) {
                setRate(currentBytesPerSecond_ + (healthyTarget - currentBytesPerSecond_) / 2);
            }
        }
    }

    void observeWrite(std::size_t bytes, uint64_t durationUs) {
        if (bytes == 0) return;

        if (durationUs >= config_.minimumMeasuredWriteUs) {
            const uint64_t sampleRate = std::min(
                config_.maximumLearnedBytesPerSecond,
                static_cast<uint64_t>(bytes) * 1'000'000ULL / durationUs);
            if (sampleRate > 0) {
                if (observedServiceBytesPerSecond_ == 0) {
                    observedServiceBytesPerSecond_ = sampleRate;
                } else {
                    observedServiceBytesPerSecond_ =
                        (observedServiceBytesPerSecond_ * 7 + sampleRate) / 8;
                }
                ++measuredWrites_;
            }
        }

        if (pressure_ != AdaptiveWritePressure::Healthy) return;
        healthyBytes_ += bytes;
        if (healthyBytes_ < config_.adjustmentWindowBytes) return;
        healthyBytes_ %= config_.adjustmentWindowBytes;
        if (observedServiceBytesPerSecond_ == 0) return;

        const uint64_t increaseStep = std::max<uint64_t>(
            currentBytesPerSecond_ / 8,
            4ULL * 1024ULL * 1024ULL);
        const uint64_t targetRate = std::clamp(
            observedServiceBytesPerSecond_ * config_.targetUtilizationPercent / 100,
            config_.minimumBytesPerSecond,
            config_.maximumLearnedBytesPerSecond);

        if (targetRate > currentBytesPerSecond_) {
            setRate(std::min(targetRate, currentBytesPerSecond_ + increaseStep));
        } else if (targetRate < currentBytesPerSecond_) {
            setRate(std::max(
                targetRate,
                currentBytesPerSecond_ - currentBytesPerSecond_ / 4));
        }
    }

    uint64_t currentBytesPerSecond() const { return currentBytesPerSecond_; }
    uint64_t observedServiceBytesPerSecond() const { return observedServiceBytesPerSecond_; }
    uint64_t minimumRateSeen() const { return minimumRateSeen_; }
    uint64_t maximumRateSeen() const { return maximumRateSeen_; }
    uint64_t dynamicMinimumRate() const { return dynamicMinimumBytesPerSecond(); }
    std::size_t rateAdjustments() const { return rateAdjustments_; }
    std::size_t pressureBackoffs() const { return pressureBackoffs_; }
    std::size_t measuredWrites() const { return measuredWrites_; }

private:
    uint64_t dynamicMinimumBytesPerSecond() const {
        if (observedServiceBytesPerSecond_ == 0) return config_.minimumBytesPerSecond;
        return std::clamp(
            observedServiceBytesPerSecond_ * config_.minimumMeasuredServicePercent / 100,
            config_.minimumBytesPerSecond,
            config_.maximumLearnedBytesPerSecond);
    }

    void setRate(uint64_t rate) {
        rate = std::clamp(
            rate,
            config_.minimumBytesPerSecond,
            config_.maximumLearnedBytesPerSecond);
        if (rate == currentBytesPerSecond_) return;
        currentBytesPerSecond_ = rate;
        minimumRateSeen_ = std::min(minimumRateSeen_, rate);
        maximumRateSeen_ = std::max(maximumRateSeen_, rate);
        ++rateAdjustments_;
    }

    AdaptiveWritePacerConfig config_;
    AdaptiveWritePressure pressure_ = AdaptiveWritePressure::Healthy;
    uint64_t currentBytesPerSecond_ = 0;
    uint64_t observedServiceBytesPerSecond_ = 0;
    uint64_t minimumRateSeen_ = 0;
    uint64_t maximumRateSeen_ = 0;
    std::size_t healthyBytes_ = 0;
    std::size_t rateAdjustments_ = 0;
    std::size_t pressureBackoffs_ = 0;
    std::size_t measuredWrites_ = 0;
};

}  // namespace clipture
