/*
 * Copyright (c) 2018 Nordic Semiconductor ASA
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 *   1. Redistributions of source code must retain the above copyright notice, this
 *   list of conditions and the following disclaimer.
 *
 *   2. Redistributions in binary form must reproduce the above copyright notice, this
 *   list of conditions and the following disclaimer in the documentation and/or
 *   other materials provided with the distribution.
 *
 *   3. Neither the name of Nordic Semiconductor ASA nor the names of other
 *   contributors to this software may be used to endorse or promote products
 *   derived from this software without specific prior written permission.
 *
 *   4. This software must only be used in or with a processor manufactured by Nordic
 *   Semiconductor ASA, or in or with a processor manufactured by a third party that
 *   is used in combination with a processor manufactured by Nordic Semiconductor.
 *
 *   5. Any software provided in binary or object form under this license must not be
 *   reverse engineered, decompiled, modified and/or disassembled.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

// Logging support
#include "internal/log.h"

// Test support
#define CATCH_CONFIG_MAIN
#include "catch2/catch.hpp"

#include "transport.h"
#include "h5_transport.h"
#include "h5.h"
#include "nrf_error.h"

#include "test_setup.h"

#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <cstdlib>
#include <thread>
#include <random>
#include <iomanip>
#include <fstream>
#include <exception>

#if defined(_MSC_VER)
// Disable warning "This function or variable may be unsafe. Consider using _dupenv_s instead."
#pragma warning(disable: 4996)
#endif

using namespace std::chrono_literals;
using std::chrono::system_clock;

std::fstream logFile(
    "test_h5_transport.txt",
    std::fstream::out | std::fstream::trunc
);

class H5TransportTestSetup
{
public:
    H5TransportTestSetup(const std::string transportName, Transport *lowerTransport): name(transportName)
    {
        transport = std::shared_ptr<test::H5TransportWrapper>(
            new test::H5TransportWrapper(lowerTransport, 250)
        );
    }

    void statusCallback(sd_rpc_app_status_t code, const char *message)
    {
        std::stringstream logEntry;
        logEntry << "[" << name << "][status] code: " << code << " message: " << message;
        DEBUG(logEntry.str());
    }

    void dataCallback(uint8_t *data, size_t length)
    {
        std::stringstream logEntry;
        incoming.assign(data, data + length);
        logEntry << "[" << name << "][data]<- " << testutil::convertToString(incoming) << " length: " << length;
        DEBUG(logEntry.str());
    }

    void logCallback(sd_rpc_log_severity_t severity, std::string message)
    {
        std::stringstream logEntry;
        logEntry << "[" << name << "][log] severity: " << severity << " message: " << message;
        DEBUG(logEntry.str());
    }

    void setup()
    {
        transport->wrappedOpen(
            std::bind(&H5TransportTestSetup::statusCallback, this, std::placeholders::_1, std::placeholders::_2),
            std::bind(&H5TransportTestSetup::dataCallback, this, std::placeholders::_1, std::placeholders::_2),
            std::bind(&H5TransportTestSetup::logCallback, this, std::placeholders::_1, std::placeholders::_2)
        );
    }

    std::shared_ptr<test::H5TransportWrapper> get()
    {
        return transport;
    }

    uint32_t wait()
    {
        return transport->waitForResult();
    }

    
    h5_state_t state() const
    {
        return transport->state();
    }

    uint32_t close() const
    {
        return transport->close();
    }

    payload_t in()
    {
        return incoming;
    }

private:
    std::shared_ptr<test::H5TransportWrapper> transport;
    payload_t incoming;
    const std::string name;
};

TEST_CASE("H5TransportWrapper")
{
    setLogCallback([](const char *message) -> void
    {
        // You should not use stdout/stderr on Windows since this will have a huge impact on
        // the application since this logger is not offloading the displaying of data
        // to a separate thread. 
        //
        // This stack overflow thread contains more info in regards to cmd.exe output:
        //   https://stackoverflow.com/questions/7404551/why-is-console-output-so-slow

        logFile << message << std::flush;
    });

    SECTION("open_close")
    {
        for (auto i = 0; i < 100; i++)
        {
            auto transportA = new VirtualUart("uartA");
            auto transportB = new VirtualUart("uartB");

            // Connect the two virtual UARTs together
            transportA->setPeer(transportB);
            transportB->setPeer(transportA);

            H5TransportTestSetup a("transportA", transportA);
            a.setup();

            H5TransportTestSetup b("transportB", transportB);
            b.setup();

            auto resultA = a.wait();
            REQUIRE(resultA == NRF_SUCCESS);
            
            auto resultB = b.wait();
            REQUIRE(resultB == NRF_SUCCESS);

            a.close();
            b.close();

            REQUIRE(a.state() == STATE_CLOSED);
            REQUIRE(b.state() == STATE_CLOSED);
        }
    }
}

TEST_CASE("H5Transport")
{
    // Setup logging
    setLogCallback([](const char *message) -> void
    {
        // You should not use stdout/stderr on Windows since this will have a huge impact on
        // the application since this logger is not offloading the displaying of data
        // to a separate thread. 
        //
        // This stack overflow thread contains more info in regards to cmd.exe output:
        //   https://stackoverflow.com/questions/7404551/why-is-console-output-so-slow

        logFile << message << std::flush;
    });

    SECTION("fail_open_invalid_inbound")
    {
        auto lowerTransport = new test::VirtualTransportSendSync();
        H5TransportTestSetup transportUnderTest("transportUnderTest", lowerTransport);
        transportUnderTest.setup();

        REQUIRE(transportUnderTest.wait() == NRF_ERROR_TIMEOUT);
        REQUIRE(transportUnderTest.state() == STATE_FAILED);
        DEBUG("Transport closed.");
    }

    SECTION("packet_recognition")
    {
        payload_t packet{ 0xff, 0x01, 0x02, 0xff, 0x01, 0x02, 0x03, 0xff };
        payload_t pattern{ 0x01, 0x02, 0x03 };

        REQUIRE(H5Transport::checkPattern(packet, 0, pattern) == false);
        REQUIRE(H5Transport::checkPattern(packet, 1, pattern) == false);
        REQUIRE(H5Transport::checkPattern(packet, 4, pattern) == true);
        REQUIRE(H5Transport::checkPattern(packet, 8, pattern) == false);
        REQUIRE(H5Transport::checkPattern(packet, 100, pattern) == false);
    }

    SECTION("response_missing")
    {
        SECTION("missing CONTROL_PKT_SYNC_RESPONSE")
        {
            // Create two virtual transports
            auto transportA = new VirtualUart("transportA");
            auto transportB = new VirtualUart("transportB");

            // Connect the two virtual UARTs together
            transportA->setPeer(transportB);
            transportB->setPeer(transportA);

            // Prevent transportB from replying at given state
            transportB->stopAt(CONTROL_PKT_SYNC);

            // Ownership of transport is transferred to H5TransportWrapper
            H5TransportTestSetup transportUnderTest("transportUnderTest", transportA);
            H5TransportTestSetup testerTransport("testerTransport", transportB);

            transportUnderTest.setup();
            testerTransport.setup();

            REQUIRE(transportUnderTest.wait() == NRF_ERROR_TIMEOUT);
            
            // H5Transport will retry n number of times if it does not
            // receive a CONTROL_PKT_SYNC_RESPONSE. 
            // If there is still no reponse, it enters STATE_FAILED.
            REQUIRE(transportUnderTest.state() == STATE_FAILED);
            testerTransport.wait();
        }

        SECTION("missing CONTROL_PKT_SYNC_RESPONSE")
        {
            // Create two virtual transports
            auto transportA = new VirtualUart("transportA");
            auto transportB = new VirtualUart("transportB");

            // Connect the two virtual UARTs together
            transportA->setPeer(transportB);
            transportB->setPeer(transportA);

            // Prevent transportB from replying at given state
            transportB->stopAt(CONTROL_PKT_SYNC);

            H5TransportTestSetup transportUnderTest("transportUnderTest", transportA);
            H5TransportTestSetup testerTransport("testerTransport", transportB);

            transportUnderTest.setup();
            testerTransport.setup();

            REQUIRE(transportUnderTest.wait() == NRF_ERROR_TIMEOUT);
            REQUIRE(transportUnderTest.state() == STATE_FAILED);
            testerTransport.wait();
        }

        SECTION("missing CONTROL_PKT_SYNC_CONFIG_RESPONSE")
        {
            // Create two virtual transports
            auto transportA = new VirtualUart("transportA");
            auto transportB = new VirtualUart("transportB");

            // Connect the two virtual UARTs together
            transportA->setPeer(transportB);
            transportB->setPeer(transportA);

            // Prevent transportB from replying at given state
            transportB->stopAt(CONTROL_PKT_SYNC_CONFIG);

            // Ownership of transport is transferred to H5TransportWrapper
            H5TransportTestSetup transportUnderTest("transportUnderTest", transportA);
            H5TransportTestSetup testerTransport("testerTransport", transportB);

            transportUnderTest.setup();
            testerTransport.setup();

            REQUIRE(transportUnderTest.wait() == NRF_ERROR_TIMEOUT);
            REQUIRE(transportUnderTest.state() == STATE_FAILED);
            testerTransport.wait();
        }
    }

    SECTION("send_receive_data")
    {
        auto transportA = new VirtualUart("uartA");
        auto transportB = new VirtualUart("uartB");

        // Connect the two virtual UARTs together
        transportA->setPeer(transportB);
        transportB->setPeer(transportA);

        // Ownership of transport is transferred to H5TransportWrapper
        H5TransportTestSetup h5TransportA("transportA", transportA);
        H5TransportTestSetup h5TransportB("transportB", transportB);

        h5TransportA.setup();
        h5TransportB.setup();

        // Wait for both transports to be opened (in HCI ACTIVE STATE)
        REQUIRE(h5TransportA.wait() == NRF_SUCCESS);
        REQUIRE(h5TransportB.wait() == NRF_SUCCESS);

        // Check that state is correct
        REQUIRE(h5TransportA.state() == STATE_ACTIVE);
        REQUIRE(h5TransportB.state() == STATE_ACTIVE);

        auto payloadToB = payload_t{ 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa };
        auto payloadToA = payload_t{ 0xbb, 0xbb, 0xbb, 0xbb, 0xbb, 0xbb };

        h5TransportA.get()->send(payloadToB);
        h5TransportB.get()->send(payloadToA);

        // Wait for data to be sent between transports
        std::this_thread::sleep_for(std::chrono::seconds(1));

        REQUIRE(payloadToB.size() == h5TransportB.in().size());
        REQUIRE(payloadToA.size() == h5TransportA.in().size());

        REQUIRE(std::equal(payloadToB.begin(), payloadToB.end(), h5TransportB.in().begin()) == true);
        REQUIRE(std::equal(payloadToA.begin(), payloadToA.end(), h5TransportA.in().begin()) == true);

        h5TransportA.close();
        REQUIRE(h5TransportA.state() == STATE_CLOSED);
        
        h5TransportB.close();
        REQUIRE(h5TransportB.state() == STATE_CLOSED);
    }
}
