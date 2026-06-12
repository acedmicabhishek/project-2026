#include "hft_simulator/feed.hpp"
#include <fstream>
#include <stdexcept>
#include <cstring>
#include <arpa/inet.h>

namespace hft {

uint64_t ITCHParser::decode_ts(const uint8_t ts[6]) {
    uint64_t ns = 0;
    for (int i = 0; i < 6; ++i)
        ns = (ns << 8) | ts[i];
    return ns;
}

std::string ITCHParser::decode_symbol(const char stock[8]) {
    std::string s(stock, 8);

    size_t end = s.find_last_not_of(' ');
    return (end == std::string::npos) ? "" : s.substr(0, end + 1);
}

size_t ITCHParser::parse_message(const uint8_t* msg, size_t available) {
    if (available < 1) return 0;

    char msg_type = static_cast<char>(msg[0]);

    switch (msg_type) {
    case 'A': {
        if (available < sizeof(ITCHAddOrder)) return 0;
        const auto* m = reinterpret_cast<const ITCHAddOrder*>(msg);
        if (on_add_order)
            on_add_order(*m, decode_ts(m->timestamp));
        return sizeof(ITCHAddOrder);
    }
    case 'D': {
        if (available < sizeof(ITCHDeleteOrder)) return 0;
        const auto* m = reinterpret_cast<const ITCHDeleteOrder*>(msg);
        if (on_delete_order)
            on_delete_order(*m, decode_ts(m->timestamp));
        return sizeof(ITCHDeleteOrder);
    }
    case 'X': {
        if (available < sizeof(ITCHOrderCancelled)) return 0;
        const auto* m = reinterpret_cast<const ITCHOrderCancelled*>(msg);
        if (on_cancel_order)
            on_cancel_order(*m, decode_ts(m->timestamp));
        return sizeof(ITCHOrderCancelled);
    }
    case 'P': {
        if (available < sizeof(ITCHTrade)) return 0;
        const auto* m = reinterpret_cast<const ITCHTrade*>(msg);
        if (on_trade)
            on_trade(*m, decode_ts(m->timestamp));
        return sizeof(ITCHTrade);
    }
    default:

        return 1;
    }
}

void ITCHParser::parse(const uint8_t* buf, size_t len) {
    size_t offset = 0;
    while (offset < len) {
        size_t consumed = parse_message(buf + offset, len - offset);
        if (consumed == 0) break;
        offset += consumed;
    }
}

void ITCHParser::parse_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("ITCHParser: cannot open " + path);

    f.seekg(0, std::ios::end);
    size_t size = static_cast<size_t>(f.tellg());
    f.seekg(0);
    std::vector<uint8_t> buf(size);
    f.read(reinterpret_cast<char*>(buf.data()), size);
    parse(buf.data(), size);
}

}
