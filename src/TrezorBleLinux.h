// SPDX-License-Identifier: BSD-3-Clause
// SPDX-FileCopyrightText: The Monero Project

#ifndef FEATHER_TREZORBLELINUX_H
#define FEATHER_TREZORBLELINUX_H

/**
 * Bluetooth Low Energy backend for Linux, built on BlueZ.
 *
 * monero's device layer deliberately keeps no dependency on any particular
 * Bluetooth stack: it exposes hw::trezor::ble::set_backend_factory() so the host
 * application supplies one. That is what this does, which is why a Linux backend
 * lives in Feather rather than in monero.
 *
 * BlueZ is driven over D-Bus, and Feather already links Qt, so QtDBus is used
 * rather than pulling libdbus or GLib into the build for a single feature.
 *
 * Installing is safe on any system: if BlueZ is absent, or no adapter is
 * present, or the user's session has no system bus, enumeration simply finds
 * nothing and Bluetooth stays unavailable rather than failing loudly.
 */
// Defined out of line only where it can do something; elsewhere the call
// compiles away entirely, so callers need no platform guards of their own and
// no stub translation unit has to be built.
#if defined(__linux__)
void installTrezorBleLinuxBackend();
#else
inline void installTrezorBleLinuxBackend() {}
#endif

#endif //FEATHER_TREZORBLELINUX_H
