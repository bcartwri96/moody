#ifndef MOODY_RX_FRAME_FIFO_H
#define MOODY_RX_FRAME_FIFO_H

#include <array>
#include <cstddef>
#include <cstdint>

template <std::size_t SlotCount>
class FrameSlotFifo {
 public:
  static_assert(SlotCount > 0, "FrameSlotFifo requires at least one slot");
  static_assert(SlotCount <= static_cast<std::size_t>(UINT8_MAX) + 1U,
                "FrameSlotFifo slot IDs must fit in uint8_t");

  bool acquireFree(uint8_t &slot) {
    for (std::size_t index = 0; index < SlotCount; ++index) {
      if (states_[index] == SlotState::free) {
        states_[index] = SlotState::held_by_reader;
        slot = static_cast<uint8_t>(index);
        return true;
      }
    }
    return false;
  }

  bool acquireSpecific(uint8_t slot) {
    if (!isValidSlot(slot) || states_[slot] != SlotState::free) {
      return false;
    }

    states_[slot] = SlotState::held_by_reader;
    return true;
  }

  bool enqueueReady(uint8_t slot) {
    if (!isValidSlot(slot) || states_[slot] != SlotState::held_by_reader ||
        ready_count_ == SlotCount) {
      return false;
    }

    ready_slots_[ready_tail_] = slot;
    ready_tail_ = nextIndex(ready_tail_);
    ++ready_count_;
    states_[slot] = SlotState::ready;
    return true;
  }

  bool dequeueReady(uint8_t &slot) {
    if (ready_count_ == 0) {
      return false;
    }

    const uint8_t next_slot = ready_slots_[ready_head_];
    if (!isValidSlot(next_slot) || states_[next_slot] != SlotState::ready) {
      return false;
    }

    ready_head_ = nextIndex(ready_head_);
    --ready_count_;
    states_[next_slot] = SlotState::held_by_reader;
    slot = next_slot;
    return true;
  }

  bool release(uint8_t slot) {
    if (!isValidSlot(slot) || states_[slot] != SlotState::held_by_reader) {
      return false;
    }

    states_[slot] = SlotState::free;
    return true;
  }

  std::size_t freeCount() const {
    std::size_t count = 0;
    for (SlotState state : states_) {
      if (state == SlotState::free) {
        ++count;
      }
    }
    return count;
  }

  std::size_t readyCount() const { return ready_count_; }

 private:
  enum class SlotState : uint8_t { free, held_by_reader, ready };

  bool isValidSlot(uint8_t slot) const {
    return static_cast<std::size_t>(slot) < SlotCount;
  }

  std::size_t nextIndex(std::size_t index) const {
    return (index + 1U) % SlotCount;
  }

  std::array<SlotState, SlotCount> states_{};
  std::array<uint8_t, SlotCount> ready_slots_{};
  std::size_t ready_head_ = 0;
  std::size_t ready_tail_ = 0;
  std::size_t ready_count_ = 0;
};

#endif
