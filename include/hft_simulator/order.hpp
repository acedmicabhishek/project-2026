#pragma once

#include <cstdint>
#include <string>

namespace hft {

enum class Side     : uint8_t { Buy = 0, Sell = 1 };
enum class OrdType  : uint8_t { Market = 0, Limit = 1, IOC = 2, FOK = 3 };
enum class OrdStatus: uint8_t { New = 0, PartialFill = 1, Filled = 2,
                                  Cancelled = 3, Rejected = 4 };

struct Order {
    uint64_t  id{0};
    std::string symbol;
    Side      side{Side::Buy};
    OrdType   type{OrdType::Limit};
    double    price{0.0};
    uint32_t  qty{0};
    uint32_t  filled_qty{0};
    OrdStatus status{OrdStatus::New};
    uint64_t  ts_ns{0};

    uint32_t leaves_qty() const noexcept { return qty - filled_qty; }
    bool     is_done()    const noexcept {
        return status == OrdStatus::Filled ||
               status == OrdStatus::Cancelled ||
               status == OrdStatus::Rejected;
    }
};

struct Fill {
    uint64_t maker_id{0};
    uint64_t taker_id{0};
    double   price{0.0};
    uint32_t qty{0};
    Side     aggressor{Side::Buy};  // taker side
    uint64_t ts_ns{0};
};

struct BookSnapshot {
    std::string symbol;
    double best_bid{0.0};
    double best_ask{0.0};
    uint32_t bid_sz{0};
    uint32_t ask_sz{0};
    uint64_t ts_ns{0};
};

} // namespace hft
