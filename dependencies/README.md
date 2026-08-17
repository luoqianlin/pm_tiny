# Bundled dependencies

PM_Tiny vendors the following source releases so Linux, Android and Windows MSVC builds do not require network access:

| Dependency | Version | Upstream | License | Source archive SHA-256 |
| --- | --- | --- | --- | --- |
| Asio | 1.30.2 | https://github.com/chriskohlhoff/asio | Boost Software License 1.0 | See `asio/PM_TINY_VENDOR.md` |
| yaml-cpp | 0.9.0 | https://github.com/jbeder/yaml-cpp | MIT | `25cb043240f828a8c51beb830569634bc7ac603978e0f69d6b63558dadefd49a` |
| nlohmann/json | 3.12.0 | https://github.com/nlohmann/json | MIT | `42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa` |
| libfort | 0.4.2 | https://github.com/seleznevae/libfort | MIT | `8f7b03f1aa526e50c9828f09490f3c844b73d5f9ca72493fe81931746f75e489` |
| utf8proc | 2.11.3 | https://github.com/JuliaStrings/utf8proc | MIT/Unicode data license | `abfed50b6d4da51345713661370290f4f4747263ee73dc90356299dfc7990c78` |

CMake verifies bundled dependency versions. Third-party tests, examples and install targets are not part of the PM_Tiny build.
