#include <slims3/slims3.hpp>

#include <cstdio>

int main() {
    slims3::Config cfg;
    cfg.endpoint = "http://127.0.0.1:9000";
    cfg.accessKey = "minioadmin";
    cfg.secretKey = "minioadmin";

    slims3::Client client(cfg);
    (void)client;

    std::printf("slims3 test_package: Client constructed OK\n");
    return 0;
}
