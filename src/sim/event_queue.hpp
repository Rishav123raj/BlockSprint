#pragma once
// The simulator's event queue (P3.3): a min-heap ordered by (time, priority class, insertion sequence).
// The insertion sequence breaks every remaining tie, so the processing order never depends on the heap's
// internals: a run is reproducible event by event.
#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace sim {

// Priority classes at equal times (lower first). Market data first is the conservative default: an
// order arriving at the same nanosecond as a data update meets the updated book.
enum class EventClass : uint8_t {
    MARKET_DATA = 0,
    ORDER_ARRIVE = 1,
    STRATEGY_MD = 2,
    STRATEGY_EXEC = 3,
    TIMER = 4,
};

// Which goes first when market data and an order arrival have the same time. DATA_FIRST (the default) is
// conservative: the order meets the updated book. ORDERS_FIRST is for sensitivity runs.
enum class TieBreak : uint8_t { DATA_FIRST, ORDERS_FIRST };

template <typename Payload>
class EventQueue {
public:
    explicit EventQueue(TieBreak tie_break = TieBreak::DATA_FIRST) : later_{tie_break} {}

    struct Event {
        uint64_t time;
        EventClass cls;
        uint64_t seq;
        Payload payload;
    };

    // Returns the event's insertion sequence number
    uint64_t push(uint64_t time, EventClass cls, Payload payload) {
        uint64_t seq = next_seq_++;
        heap_.push_back(Event{time, cls, seq, std::move(payload)});
        std::push_heap(heap_.begin(), heap_.end(), later_);
        return seq;
    }

    bool empty() const { return heap_.empty(); }
    size_t size() const { return heap_.size(); }

    const Event& top() const {
        if (heap_.empty()) {
            throw std::out_of_range("EventQueue::top on an empty queue");
        }
        return heap_.front();
    }

    Event pop() {
        if (heap_.empty()) {
            throw std::out_of_range("EventQueue::pop on an empty queue");
        }
        std::pop_heap(heap_.begin(), heap_.end(), later_);  // the earliest event moves to the back
        Event e = std::move(heap_.back());
        heap_.pop_back();
        return e;
    }

private:
    struct Later {
        TieBreak tie_break;

        // ORDERS_FIRST swaps the first two classes
        uint8_t rank(EventClass c) const {
            const auto r = static_cast<uint8_t>(c);
            return tie_break == TieBreak::ORDERS_FIRST && r < 2 ? static_cast<uint8_t>(1 - r) : r;
        }
        bool operator()(const Event& a, const Event& b) const {
            if (a.time != b.time) return a.time > b.time;
            if (a.cls != b.cls) return rank(a.cls) > rank(b.cls);
            return a.seq > b.seq;
        }
    };

    std::vector<Event> heap_;  // a binary heap (std::push_heap / pop_heap) with the earliest event in front
    Later later_;
    uint64_t next_seq_ = 0;
};

} // namespace sim
