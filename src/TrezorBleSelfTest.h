// SPDX-License-Identifier: BSD-3-Clause
// SPDX-FileCopyrightText: The Monero Project

#ifndef FEATHER_TREZORBLESELFTEST_H
#define FEATHER_TREZORBLESELFTEST_H

/**
 * Headless Bluetooth self test, reached with `feather --test-ble`.
 *
 * Connecting a Trezor over Bluetooth involves a scan, a connection to a
 * rotating private address, a bonding ceremony the user completes on the device
 * itself, and only then a protocol handshake. Any of those can fail, and when
 * they fail behind the wallet wizard all the user sees is that no device was
 * found. This runs the very same transport the wizard uses, prints each stage as
 * it happens, and exits with a status, so a failure can be diagnosed without
 * driving the interface by hand.
 *
 * It deliberately calls the production code path rather than reimplementing it,
 * so what it proves is what the wizard will do.
 *
 * @param seconds how long to keep scanning for a device.
 * @return process exit code: 0 on a complete round trip, non-zero otherwise.
 */
int runTrezorBleSelfTest(int seconds);

#endif //FEATHER_TREZORBLESELFTEST_H
