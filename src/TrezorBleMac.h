// SPDX-License-Identifier: BSD-3-Clause
// SPDX-FileCopyrightText: The Monero Project

#ifndef FEATHER_TREZORBLEMAC_H
#define FEATHER_TREZORBLEMAC_H

/**
 * Bluetooth Low Energy backend for macOS, built on CoreBluetooth.
 *
 * As with the Linux backend, this lives in Feather rather than monero because
 * monero's device layer keeps no dependency on any particular Bluetooth stack
 * and instead lets the host application install one through
 * hw::trezor::ble::set_backend_factory().
 *
 * CoreBluetooth is an Objective-C framework, so the implementation is
 * Objective-C++ and everything Apple-specific stays behind this one function.
 *
 * Note there is no explicit pairing step. macOS raises the security level by
 * itself when a characteristic demands an authenticated link, which is the same
 * reason the Trezor's own client requests pairing only on Android.
 */
// Defined out of line only where it can do something; elsewhere the call
// compiles away entirely, so callers need no platform guards of their own and
// no stub translation unit has to be built.
#if defined(__APPLE__)
void installTrezorBleMacBackend();
#else
inline void installTrezorBleMacBackend() {}
#endif

#endif //FEATHER_TREZORBLEMAC_H
