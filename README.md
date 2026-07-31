# Feather Wallet — Trezor Safe 7 fork

> **This is a fork of [feather-wallet/feather](https://github.com/feather-wallet/feather) that adds Trezor Safe 7 support.**
> See [Trezor Safe 7 support](#trezor-safe-7-support) below. Everything else is unchanged from upstream.

Feather is a free Monero desktop wallet for Linux, Tails, macOS and Windows. It is written in C++ with the Qt framework.

- **easy-to-use**, **small** and **fast** - Feather runs well on any modern hardware, including virtual machines and live operating systems.
- **beginner friendly**, but also caters to advanced Monero users by providing a [feature set](https://docs.featherwallet.org/guides/features) that is on par with the official CLI.
- ships with **sane defaults** that suit most users, but can also be configured for high or uncommon threat models.
- serves as a testing grounds for **experimental features** that may later be adopted in the reference wallets.

## Download

You can download Feather from **[featherwallet.org](https://featherwallet.org/download/)** or **[GitHub](https://github.com/feather-wallet/feather/releases)**.

If you need help installing Feather, check the [installation documentation](https://docs.featherwallet.org/).

We recommend that you verify downloads with GPG. Releases are signed with our [release signing key](https://docs.featherwallet.org/guides/release-signing-key). The fingerprint is:

```
8185 E158 A333 30C7 FD61 BC0D 1F76 E155 CEFB A71C
```

## Trezor Safe 7 support

Upstream Feather cannot talk to a Trezor Safe 7. This fork can.

### Why it needed changing

The Safe 7 (internal model `T3W1`) uses the same USB vendor/product id as the
Model T, Safe 3 and Safe 5, and Monero's device layer has no model whitelist, so
the device is *detected* by unmodified Feather. It just cannot be *talked to*.

Safe 7 firmware is built with the `thp` feature, which replaces the legacy
Codec v1 framing (`##` / `?##`) with the **Trezor-Host Protocol v2** — an
encrypted, authenticated channel built on the Noise `XX` pattern. That switch is
a compile-time either/or in the firmware, so a Safe 7 does not serve the legacy
codec at all, and Monero's device layer only implemented the legacy codec. There
was no fallback to fall back to.

### What was added

A complete THP v2 client in the Monero submodule
([obnoxiousmods/monero](https://github.com/obnoxiousmods/monero), branch
`v0.18.5.1-safe7`), under `src/device_trezor/trezor/`:

| File | Contents |
|------|----------|
| `thp_curve25519.{hpp,cpp}` | X25519 against an arbitrary base point, and the Elligator2 map. Neither OpenSSL nor libsodium exposes these in the form THP needs. |
| `thp_crypto.{hpp,cpp}` | Noise HKDF, AES-256-GCM with Noise nonces, and the CPace255 generator. |
| `thp_wire.{hpp,cpp}` | Transport layer: CRC-32, 64-byte packet segmentation, control bytes, alternating-bit sequencing. |
| `protocol_thp.{hpp,cpp}` | Channel allocation, the Noise handshake, CodeEntry pairing, credential issuance and storage, sessions, encrypted transport. |

`ProtocolV1` is untouched. Which protocol to use is decided by probing the
device on connect, so **Trezor One, Model T, Safe 3 and Safe 5 continue to work
exactly as before**.

### Pairing

The first time you connect a Safe 7, the device shows a **six-digit pairing
code** and Feather prompts you to type it in. This is not a formality: typing
the code back is what completes the CPace exchange and rules out a
man-in-the-middle on the USB connection. It cannot be confirmed on the device
alone the way a passphrase can.

After pairing, Feather stores the resulting credential and reconnects without
prompting. The credential lives outside your wallet file, since it belongs to
the (host, device) pair rather than to a wallet:

* Windows — `%APPDATA%\feather\trezor_thp_credentials.json`
* Linux/macOS — `$XDG_CONFIG_HOME/feather/trezor_thp_credentials.json` (or `~/.config/feather/...`)

Deleting that file simply means pairing again. It contains no wallet keys.

### Known limitations

* **USB only.** Bluetooth is a separate subsystem and is not implemented.
* **CodeEntry pairing only.** Production Safe 7 firmware enables no other
  method — `SkipPairing`, `QrCode` and `NFC` are gated behind debug builds
  upstream — so the other methods would be untestable and are not implemented.

### Building for Windows

Cross-compiled from Linux (tested on Arch in WSL2) using the standard
`contrib/depends` route:

```bash
# host toolchain: base-devel cmake ninja git python protobuf mingw-w64-gcc
make -C contrib/depends HOST=x86_64-w64-mingw32 -j"$(nproc)"

cmake -B build \
  -DCMAKE_TOOLCHAIN_FILE=contrib/depends/x86_64-w64-mingw32/toolchain.cmake \
  -DARCH=x86-64 -DSTACK_TRACE=OFF
cmake --build build -j"$(nproc)"
```

> **Building under WSL:** run `make` with a sanitised `PATH`. WSL appends the
> Windows `PATH`, which contains directories with spaces and parentheses
> (`/mnt/c/Program Files (x86)/...`); `contrib/depends` interpolates `PATH`
> unquoted into a shell command, so Qt's configure step dies with a shell syntax
> error. Prefix the build with
> `env -i PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin HOME="$HOME"`.

### Tests

THP has unit test coverage in the Monero submodule
(`tests/unit_tests/trezor_thp.cpp`): X25519 and Elligator2 against RFC 7748 and
reference vectors, the Noise HKDF and AEAD construction including tag, nonce and
associated-data rejection, the CPace generator and exchange, CRC-32, message
encoding and reassembly with checksum rejection, the fixed handshake sequence
bits, and credential persistence.

Vectors are generated from the `trezorlib` reference implementation by
`contrib/thp/gen_vectors.py` rather than written by hand, so they can be
regenerated if the protocol changes.

## Resources

* [Official Site](https://featherwallet.org)
* [Documentation](https://docs.featherwallet.org)
* [Git Repository](https://github.com/feather-wallet/feather)
* [Matrix](https://matrix.to/#/#feather:monero.social)
* IRC: `#feather` on [OFTC](https://www.oftc.net/)
* Mail: dev@featherwallet.org

If you need help with your wallet, please contact us via Matrix or IRC.
If you don't have an IRC client, you can join the room via [webchat](https://webchat.oftc.net/?randomnick=1&channels=feather).
If you don’t receive a response immediately please idle in the room.

## Release Builds

To learn how to run a bootstrappable release build, see: [contrib/guix/README.md](https://github.com/feather-wallet/feather/blob/master/contrib/guix/README.md)

For release attestations, see the [feather-sigs](http://github.com/feather-wallet/feather-sigs) repo.

For release policy, see: [RELEASE.md](https://github.com/feather-wallet/feather/blob/master/RELEASE.md)

## Development

If you are looking to set up a development environment for Feather, see [HACKING.md](https://github.com/feather-wallet/feather/blob/master/HACKING.md).

It is highly recommended that you join our Matrix or IRC channel if you are hacking on Feather.
Idling in this channel is the best way to stay updated on best practices and new developments.

For information on how Feather is maintained, see: [MAINTENANCE.md](https://github.com/feather-wallet/feather/blob/master/MAINTENANCE.md)

To report a security vulnerability, see: [SECURITY.md](https://github.com/feather-wallet/feather/blob/master/SECURITY.md)

## License

Feather is free and open-source software, [licensed under BSD-3](https://raw.githubusercontent.com/feather-wallet/feather/master/LICENSE).

Copyright (c) 2020-2026, The Monero Project
