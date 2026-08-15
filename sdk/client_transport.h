#ifndef PM_TINY_CLIENT_TRANSPORT_H
#define PM_TINY_CLIENT_TRANSPORT_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pm_tiny {
namespace sdk_detail {

class client_transport {
public:
    virtual ~client_transport() = default;
    virtual void connect() = 0;
    virtual void send(const std::vector<std::uint8_t> &wire) = 0;
    virtual void disconnect() = 0;
    virtual void cancel() = 0;
};

std::unique_ptr<client_transport> make_client_transport(
        const std::string &endpoint, bool uds_abstract_namespace);

} // namespace sdk_detail
} // namespace pm_tiny

#endif
