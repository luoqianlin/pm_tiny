#include "core/protocol_v2.h"

#include <cassert>
#include <cstdint>

int main() {
    pm_tiny::protocol_message message;
    message.type = 0x25;
    message.flags = pm_tiny::protocol_flag_response;
    message.request_id = 0x10203040;
    message.payload = {0, 1, 2, 0xff};
    auto encoded = pm_tiny::protocol_encode(message);
    assert(encoded.size() == 20);
    assert(encoded[0] == 'P' && encoded[1] == 'M' && encoded[2] == 'T' && encoded[3] == '2');
    assert(encoded[6] == 0 && encoded[7] == 0x25);
    assert(encoded[8] == 0x10 && encoded[9] == 0x20 && encoded[10] == 0x30 && encoded[11] == 0x40);

    pm_tiny::protocol_decoder decoder;
    for (auto byte : encoded) decoder.feed(&byte, 1);
    assert(!decoder.empty());
    auto decoded = decoder.pop();
    assert(decoded.type == message.type);
    assert(decoded.flags == message.flags);
    assert(decoded.request_id == message.request_id);
    assert(decoded.payload == message.payload);

    pm_tiny::protocol_message second_message;
    second_message.type = 0x29;
    second_message.request_id = 0x50607080;
    auto second_encoded = pm_tiny::protocol_encode(second_message);
    std::vector<std::uint8_t> coalesced = encoded;
    coalesced.insert(coalesced.end(), second_encoded.begin(), second_encoded.end());
    pm_tiny::protocol_decoder coalesced_decoder;
    coalesced_decoder.feed(coalesced.data(), coalesced.size());
    assert(coalesced_decoder.pop().request_id == message.request_id);
    assert(coalesced_decoder.pop().request_id == second_message.request_id);
    assert(coalesced_decoder.empty());

    auto malformed = encoded;
    malformed[0] = 'X';
    bool rejected = false;
    try { pm_tiny::protocol_decoder().feed(malformed.data(), malformed.size()); }
    catch (const pm_tiny::protocol_error &) { rejected = true; }
    assert(rejected);

    auto invalid_flags = message;
    invalid_flags.flags = 0x80;
    rejected = false;
    try { pm_tiny::protocol_encode(invalid_flags); }
    catch (const pm_tiny::protocol_error &) { rejected = true; }
    assert(rejected);

    invalid_flags.flags = pm_tiny::protocol_flag_more;
    rejected = false;
    try { pm_tiny::protocol_encode(invalid_flags); }
    catch (const pm_tiny::protocol_error &) { rejected = true; }
    assert(rejected);
    return 0;
}
