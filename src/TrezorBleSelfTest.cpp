// SPDX-License-Identifier: BSD-3-Clause
// SPDX-FileCopyrightText: The Monero Project

#include "TrezorBleSelfTest.h"

#include <cstdio>
#include <memory>
#include <string>

#include "misc_log_ex.h"
#include "device_trezor/trezor/transport.hpp"
#include "device_trezor/trezor/transport_ble.hpp"

namespace {

// Feather is a GUI subsystem binary, so its standard output is not reliably
// attached to whatever launched it. Everything is therefore mirrored to a file
// next to the wallet logs, which is what makes this usable for diagnosis.
FILE *g_log = nullptr;

void emit(const std::string &line) {
    std::printf("%s\n", line.c_str());
    std::fflush(stdout);
    if (g_log) {
        std::fprintf(g_log, "%s\n", line.c_str());
        std::fflush(g_log);
    }
}

void step(const char *text) { emit(std::string("\n[") + text + "]"); }

void detail(const std::string &text) { emit("    " + text); }

} // namespace

int runTrezorBleSelfTest(int seconds) {
    g_log = std::fopen("ble-selftest.log", "w");

    // Turn the transport's own logging all the way up and send it to the same
    // console, so a failure inside the backend is visible here rather than
    // silently disappearing.
    mlog_configure("", true);
    mlog_set_log_level(4);

    emit("Feather Trezor Bluetooth self test");
    emit("==================================");

    using namespace hw::trezor;

    step("1/4 installing the platform backend");
    ble::install_default_backend();
    if (!ble::has_backend()) {
        detail("no Bluetooth backend available on this system");
        return 2;
    }
    detail("ok");

    // Tell the backend this is a deliberate hunt for a Bluetooth device, so it
    // scans properly instead of doing the brief cached scan that keeps USB
    // enumeration responsive.
    ble::set_active_search(true);

    step("2/4 scanning for a Trezor");
    detail("put the device in Bluetooth pairing mode now");
    t_transport_vect devices;
    try {
        BleTransport probe;
        probe.enumerate(devices);
    } catch (const std::exception &e) {
        detail(std::string("scan failed: ") + e.what());
        return 1;
    }

    if (devices.empty()) {
        detail("no Trezor is advertising");
        detail("check Bluetooth is enabled on the device and it is in pairing mode");
        return 1;
    }
    for (const auto &d : devices) {
        detail("found " + d->get_path());
    }

    step("3/4 connecting, bonding and running the protocol handshake");
    detail("confirm the six digit code on the Trezor when it appears");
    auto transport = devices.front();
    try {
        // open() performs the whole sequence the wizard depends on: connect,
        // discover the service, bond, subscribe, then the THP handshake.
        transport->open();
    } catch (const std::exception &e) {
        detail(std::string("FAILED: ") + e.what());
        return 1;
    }
    detail("connected");

    step("4/4 verifying the link answers");
    bool alive = false;
    try {
        alive = transport->ping();
    } catch (const std::exception &e) {
        detail(std::string("ping threw: ") + e.what());
    }
    detail(alive ? "device answered" : "device did not answer a ping");

    try {
        transport->close();
    } catch (const std::exception &e) {
        detail(std::string("close threw: ") + e.what());
    }

    emit("----------------------------------------");
    if (alive) {
        emit("SUCCESS: Bluetooth is fully working.");
        if (g_log) std::fclose(g_log);
        return 0;
    }
    emit("Connected, but the device did not answer.");
    if (g_log) std::fclose(g_log);
    return 1;
}
