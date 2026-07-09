#include "doctest.h"
#include "slims3/slims3.hpp"

TEST_CASE("public header compiles and Result semantics hold") {
    slims3::Result r;
    CHECK(static_cast<bool>(r));  // default = success
    r.error.kind = slims3::ErrorKind::transport;
    CHECK_FALSE(static_cast<bool>(r));
}
