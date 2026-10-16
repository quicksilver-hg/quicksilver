// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#include <crypto/cuckatoo/gpu/listing_format.h>

#include <boost/test/unit_test.hpp>

BOOST_AUTO_TEST_SUITE(opencl_devices_tests)

BOOST_AUTO_TEST_CASE(pci_id_is_fixed_width_lowercase)
{
    BOOST_CHECK_EQUAL(qsgpu::FormatPciId(0, 4, 0, 0), "pci:0000:04:00.0");
    BOOST_CHECK_EQUAL(qsgpu::FormatPciId(0x1, 0xab, 0x1f, 7), "pci:0001:ab:1f.7");
}

BOOST_AUTO_TEST_CASE(listing_line_puts_name_last_and_omits_reason_when_ok)
{
    qsgpu::ListingFields d{};
    d.backend = "opencl"; d.id = "pci:0000:04:00.0"; d.type = "gpu";
    d.mem = 8589934592ULL; d.status = "ok";
    d.name = "AMD Radeon RX 480 Graphics (radeonsi, polaris10)";
    BOOST_CHECK_EQUAL(qsgpu::FormatListingLine(d),
        "device backend=opencl id=pci:0000:04:00.0 type=gpu integrated=0 mem=8589934592 status=ok "
        "name=AMD Radeon RX 480 Graphics (radeonsi, polaris10)");
}

BOOST_AUTO_TEST_CASE(failure_reason_is_one_token)
{
    qsgpu::ListingFields d{};
    d.backend = "opencl"; d.id = "opencl:1:0"; d.type = "gpu";
    d.integrated = true; d.status = "compile-failed";
    d.reason = "error: use of undeclared identifier\nline 2";
    d.name = "Intel(R) HD Graphics 4600";
    BOOST_CHECK_EQUAL(qsgpu::FormatListingLine(d),
        "device backend=opencl id=opencl:1:0 type=gpu integrated=1 mem=0 status=compile-failed "
        "reason=error:_use_of_undeclared_identifier_line_2 name=Intel(R) HD Graphics 4600");
}

BOOST_AUTO_TEST_CASE(newlines_in_a_device_name_cannot_forge_a_line)
{
    qsgpu::ListingFields d{};
    d.backend = "opencl"; d.id = "pci:0000:01:00.0"; d.type = "gpu"; d.status = "ok";
    d.name = "Evil\ndevice backend=opencl id=pci:0000:09:00.0";
    const std::string line = qsgpu::FormatListingLine(d);
    BOOST_CHECK(line.find('\n') == std::string::npos);
    BOOST_CHECK(line.find('\r') == std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()
