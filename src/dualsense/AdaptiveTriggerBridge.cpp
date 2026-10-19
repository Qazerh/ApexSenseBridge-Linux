#include "dualsense/AdaptiveTriggerBridge.h"

#include "dualsense/AdaptiveTriggerTranslation.h"
#include "dualsense/EffectStrength.h"

#include <algorithm>
#include <cstdio>
#include <iostream>

namespace asb::dualsense {

AdaptiveTriggerBridge::AdaptiveTriggerBridge(flydigi::Apex5Device& device, unsigned strengthPercent)
    : device_(device), strengthPercent_(strengthPercent) {}

void AdaptiveTriggerBridge::handle(const DualSenseFeedback& feedback) {
    if (failed_.load(std::memory_order_relaxed) || feedback.kind != FeedbackKind::HidOutput) return;

    constexpr std::uint8_t kRightTrigger = 0x04;
    constexpr std::uint8_t kLeftTrigger = 0x08;
    if ((feedback.enableBits1 & kRightTrigger) != 0) {
        apply(TriggerSide::Right, feedback.enableBits1, feedback.rightTriggerEffect);
    }
    if ((feedback.enableBits1 & kLeftTrigger) != 0) {
        apply(TriggerSide::Left, feedback.enableBits1, feedback.leftTriggerEffect);
    }
}

void AdaptiveTriggerBridge::recordBlock(TriggerSide side, std::uint8_t enableBits,
                                        const std::array<std::uint8_t, 11>& effect) {
    constexpr std::size_t kMaximumDistinctBlocks = 32;
    std::array<std::uint8_t, 12> block{};
    block[0] = enableBits;
    std::copy(effect.begin(), effect.end(), block.begin() + 1);
    const auto index = side == TriggerSide::Left ? 0U : 1U;

    std::string line;
    std::size_t number = 0;
    {
        std::lock_guard lock(stateMutex_);
        if (block == lastBlock_[index]) return;
        lastBlock_[index] = block;
        auto& seen = seenBlocks_[index];
        if (seen.size() >= kMaximumDistinctBlocks ||
            std::find(seen.begin(), seen.end(), block) != seen.end()) return;
        seen.push_back(block);
        number = seen.size();
        char text[8];
        for (std::size_t i = 0; i < block.size(); ++i) {
            std::snprintf(text, sizeof text, "%02X", block[i]);
            if (i == 1) line += " fx=";
            else if (i > 1) line += ' ';
            else line += "en=";
            line += text;
        }
        blockLog_[index].push_back(line);
    }
    if (log_) {
        log_(std::string("[triggers] ") + (index == 0 ? "lt" : "rt") + " block #" + std::to_string(number) + ' ' + line);
    } else {
        std::cerr << "[triggers] " << (index == 0 ? "lt" : "rt") << " block #" << number
                  << ' ' << line << '\n';
    }
}

void AdaptiveTriggerBridge::apply(TriggerSide side, std::uint8_t enableBits,
                                  const std::array<std::uint8_t, 11>& effect) {
    recordBlock(side, enableBits, effect);

    // Like a DualSense, keep the current effect: games such as Call of Duty interleave
    // empty blocks with active effects, and releasing on them makes the trigger oscillate.
    if (effect[0] == 0) {
        neutral_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    auto translated = translateAdaptiveTrigger(side, effect, 0);
    if (!translated) {
        unsupported_.fetch_add(1, std::memory_order_relaxed);
        (side == TriggerSide::Left ? lastUnsupportedLeftType_ : lastUnsupportedRightType_)
            .store(effect[0], std::memory_order_relaxed);
        std::lock_guard lock(stateMutex_);
        ++unsupportedByType_[effect[0]];
        return;
    }
    (side == TriggerSide::Left ? lastLeftType_ : lastRightType_)
        .store(effect[0], std::memory_order_relaxed);
    const auto& identity = device_.identity();
    std::optional<BowEmulation> bow;
    if (identity && !identity->isApex4()) bow = describeBowEmulation(side, effect);
    {
        std::lock_guard lock(stateMutex_);
        auto& armed = armedBow_[side == TriggerSide::Left ? 0U : 1U];
        if (bow && armed && armed->effect == effect) {
            deduplicated_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (bow) armed = ArmedBow{effect, *bow, false};
        else armed.reset();
        bowArmed_.store(armedBow_[0] || armedBow_[1], std::memory_order_relaxed);
    }
    if (bow) *translated = bow->draw;
    *translated = scaleTriggerStrength(*translated, strengthPercent_);
    {
        std::lock_guard lock(stateMutex_);
        auto& previous = side == TriggerSide::Left ? lastLeft_ : lastRight_;
        if (previous == translated) {
            deduplicated_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }

    if (!send(*translated, effect[0])) return;
    translated_.fetch_add(1, std::memory_order_relaxed);
    if (translated->mode == TriggerMode::Normal) {
        normal_.fetch_add(1, std::memory_order_relaxed);
    } else {
        active_.fetch_add(1, std::memory_order_relaxed);
    }
}

bool AdaptiveTriggerBridge::send(const ForceTriggerCommand& command, std::uint8_t dualSenseType) {
    std::string writeError;
    if (!device_.queueTriggerRaw(command, writeError)) {
        writeFailures_.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard lock(errorMutex_);
            error_ = std::move(writeError);
        }
        failed_.store(true, std::memory_order_relaxed);
        return false;
    }
    const bool left = command.side == TriggerSide::Left;
    std::lock_guard lock(stateMutex_);
    (left ? lastLeft_ : lastRight_) = command;
    if (command.mode != TriggerMode::Normal) {
        if (dualSenseType != 0) (left ? lastActiveLeftType_ : lastActiveRightType_) = dualSenseType;
        (left ? lastActiveLeft_ : lastActiveRight_) = command;
    }
    return true;
}

void AdaptiveTriggerBridge::updateTriggerPositions(std::uint8_t left, std::uint8_t right) {
    if (!bowArmed_.load(std::memory_order_relaxed) || failed_.load(std::memory_order_relaxed)) return;
    for (const auto side : {TriggerSide::Left, TriggerSide::Right}) {
        const auto position = side == TriggerSide::Left ? left : right;
        std::optional<ForceTriggerCommand> command;
        {
            std::lock_guard lock(stateMutex_);
            auto& armed = armedBow_[side == TriggerSide::Left ? 0U : 1U];
            if (!armed) continue;
            if (!armed->ruptured && position >= armed->emulation.ruptureAxis) {
                command = armed->emulation.rupture;
                armed->ruptured = true;
            } else if (armed->ruptured && position <= armed->emulation.rearmAxis) {
                command = armed->emulation.draw;
                armed->ruptured = false;
            }
        }
        if (command && !send(scaleTriggerStrength(*command, strengthPercent_), 0)) return;
    }
}

bool AdaptiveTriggerBridge::failed() const noexcept {
    return failed_.load(std::memory_order_relaxed);
}

std::string AdaptiveTriggerBridge::error() const {
    std::lock_guard lock(errorMutex_);
    return error_;
}

AdaptiveTriggerBridgeStats AdaptiveTriggerBridge::stats() const {
    std::lock_guard lock(stateMutex_);
    std::vector<std::pair<std::uint8_t, std::uint64_t>> byType;
    for (unsigned type = 0; type < unsupportedByType_.size(); ++type) {
        if (unsupportedByType_[type] != 0) {
            byType.emplace_back(static_cast<std::uint8_t>(type), unsupportedByType_[type]);
        }
    }
    std::sort(byType.begin(), byType.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    if (byType.size() > 4) byType.resize(4);
    return {translated_.load(std::memory_order_relaxed),
            active_.load(std::memory_order_relaxed),
            normal_.load(std::memory_order_relaxed),
            deduplicated_.load(std::memory_order_relaxed),
            neutral_.load(std::memory_order_relaxed),
            unsupported_.load(std::memory_order_relaxed),
            writeFailures_.load(std::memory_order_relaxed),
            lastLeftType_.load(std::memory_order_relaxed),
            lastRightType_.load(std::memory_order_relaxed),
            lastLeft_, lastRight_,
            lastActiveLeftType_, lastActiveRightType_,
            lastActiveLeft_, lastActiveRight_,
            lastUnsupportedLeftType_.load(std::memory_order_relaxed),
            lastUnsupportedRightType_.load(std::memory_order_relaxed),
            std::move(byType), blockLog_[0], blockLog_[1]};
}

} // namespace asb::dualsense
