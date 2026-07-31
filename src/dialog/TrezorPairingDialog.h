// SPDX-License-Identifier: BSD-3-Clause
// SPDX-FileCopyrightText: The Monero Project

#ifndef FEATHER_TREZORPAIRINGDIALOG_H
#define FEATHER_TREZORPAIRINGDIALOG_H

#include <QInputDialog>
#include <QMessageBox>
#include <QRegularExpression>
#include <QString>

#include <optional>

/**
 * Prompt for the six-digit pairing code a Trezor Safe 7 displays.
 *
 * Devices speaking the Trezor-Host Protocol show this code the first time a
 * given host connects. Typing it back is what completes the CPace exchange and
 * rules out a man-in-the-middle on the USB connection, so it has to be entered
 * on the host rather than confirmed on the device.
 *
 * Returns std::nullopt if the user cancelled.
 *
 * This lives in a header shared by MainWindow and WindowManager because the
 * request can arrive on two different paths: opening an existing device wallet
 * goes through Wallet, while creating one goes through WalletManager.
 */
inline std::optional<QString> promptTrezorPairingCode()
{
    while (true) {
        bool ok = false;
        QString code = QInputDialog::getText(
                nullptr, "Trezor Pairing Code",
                "Your Trezor is displaying a six-digit pairing code.\n\n"
                "Enter it here to finish pairing. This is only needed the first "
                "time you connect this device to Feather.",
                QLineEdit::EchoMode::Normal, "", &ok);

        if (!ok) {
            return std::nullopt;
        }

        code = code.trimmed();
        static const QRegularExpression sixDigits{QStringLiteral("^[0-9]{6}$")};
        if (sixDigits.match(code).hasMatch()) {
            return code;
        }

        QMessageBox::warning(nullptr, "Trezor Pairing Code",
                             "The pairing code must be exactly six digits.");
    }
}

#endif //FEATHER_TREZORPAIRINGDIALOG_H
