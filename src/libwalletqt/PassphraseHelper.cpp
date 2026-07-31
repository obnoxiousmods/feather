// SPDX-License-Identifier: BSD-3-Clause
// SPDX-FileCopyrightText: The Monero Project

#include "PassphraseHelper.h"
#include <QDebug>

std::optional<std::string> PassphraseHelper::onDevicePassphraseRequest(bool & on_device)
{
    qDebug() << __FUNCTION__;
    QMutexLocker locker(&m_mutex_pass);
    m_passphrase_on_device = true;
    m_passphrase_abort = false;

    if (m_prompter != nullptr){
        m_prompter->onWalletPassphraseNeeded(on_device);
    }

    m_cond_pass.wait(&m_mutex_pass);

    if (m_passphrase_abort)
    {
        throw std::runtime_error("Passphrase entry abort");
    }

    on_device = m_passphrase_on_device;
    if (!on_device) {
        auto tmpPass = m_passphrase.toStdString();
        m_passphrase = QString();
        return std::optional<std::string>(tmpPass);
    } else {
        return std::optional<std::string>();
    }
}

std::optional<std::string> PassphraseHelper::onDevicePairingCodeRequest()
{
    qDebug() << __FUNCTION__;
    QMutexLocker locker(&m_mutex_pairing);
    m_pairing_abort = false;

    if (m_prompter != nullptr){
        m_prompter->onWalletPairingCodeNeeded();
    }

    // The device holds the channel open while the user reads the code off its
    // screen, so this blocks the wallet thread until the UI answers.
    m_cond_pairing.wait(&m_mutex_pairing);

    if (m_pairing_abort)
    {
        throw std::runtime_error("Pairing code entry abort");
    }

    auto code = m_pairing_code.toStdString();
    m_pairing_code = QString();
    return std::optional<std::string>(code);
}

void PassphraseHelper::onPairingCodeEntered(const QString &code, bool entry_abort)
{
    qDebug() << __FUNCTION__;
    QMutexLocker locker(&m_mutex_pairing);
    m_pairing_code = code;
    m_pairing_abort = entry_abort;

    m_cond_pairing.wakeAll();
}

void PassphraseHelper::onPassphraseEntered(const QString &passphrase, bool enter_on_device, bool entry_abort)
{
    qDebug() << __FUNCTION__;
    QMutexLocker locker(&m_mutex_pass);
    m_passphrase = passphrase;
    m_passphrase_abort = entry_abort;
    m_passphrase_on_device = enter_on_device;

    m_cond_pass.wakeAll();
}
