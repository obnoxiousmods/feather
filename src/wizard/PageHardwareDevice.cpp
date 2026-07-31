// SPDX-License-Identifier: BSD-3-Clause
// SPDX-FileCopyrightText: The Monero Project

#include "PageHardwareDevice.h"
#include "ui_PageHardwareDevice.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QVBoxLayout>
#include <QPushButton>

#include "WalletWizard.h"

PageHardwareDevice::PageHardwareDevice(WizardFields *fields, QWidget *parent)
        : QWizardPage(parent)
        , ui(new Ui::PageHardwareDevice)
        , m_fields(fields)
{
    ui->setupUi(this);

    ui->combo_deviceType->addItem("Ledger", DeviceType::LEDGER);
    ui->combo_deviceType->addItem("Trezor", DeviceType::TREZOR);

    // Bluetooth exists only on the Trezor Safe 7, so the option is hidden until
    // a Trezor is selected rather than being offered where it cannot work.
    m_checkBluetooth = new QCheckBox("Connect over Bluetooth (Trezor Safe 7)", this);
    m_checkBluetooth->setToolTip(
            "Pair with a Trezor Safe 7 over Bluetooth instead of USB.\n"
            "Enable Bluetooth on the device and put it in pairing mode first.");
    if (auto *pageLayout = qobject_cast<QVBoxLayout *>(this->layout())) {
        pageLayout->addWidget(m_checkBluetooth);
    }

    connect(ui->combo_deviceType, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &PageHardwareDevice::onDeviceTypeChanged);
    connect(ui->btnOptions, &QPushButton::clicked, this, &PageHardwareDevice::onOptionsClicked);

    onDeviceTypeChanged();
}

void PageHardwareDevice::onDeviceTypeChanged() {
    const auto type = static_cast<DeviceType>(ui->combo_deviceType->currentData().toInt());
    const bool isTrezor = (type == DeviceType::TREZOR);
    m_checkBluetooth->setVisible(isTrezor);
    if (!isTrezor) {
        m_checkBluetooth->setChecked(false);
    }
}

void PageHardwareDevice::initializePage() {
    ui->radioNewWallet->setChecked(true);
}

int PageHardwareDevice::nextId() const {
    if (m_fields->showSetRestoreHeightPage) {
        return WalletWizard::Page_SetRestoreHeight;
    }

    return WalletWizard::Page_WalletFile;
}

bool PageHardwareDevice::validatePage() {
    m_fields->deviceType = static_cast<DeviceType>(ui->combo_deviceType->currentData().toInt());
    m_fields->showSetRestoreHeightPage = ui->radioRestoreWallet->isChecked();
    m_fields->useBluetooth = m_checkBluetooth->isVisible() && m_checkBluetooth->isChecked();
    return true;
}

bool PageHardwareDevice::isComplete() const {
    return true;
}

void PageHardwareDevice::onOptionsClicked() {
    QDialog dialog(this);
    dialog.setWindowTitle("Options");

    QVBoxLayout layout;
    QCheckBox check_subaddressLookahead("Set subaddress lookahead");
    check_subaddressLookahead.setChecked(m_fields->showSetSubaddressLookaheadPage);

    layout.addWidget(&check_subaddressLookahead);
    QDialogButtonBox buttons(QDialogButtonBox::Ok);
    layout.addWidget(&buttons);
    dialog.setLayout(&layout);
    connect(&buttons, &QDialogButtonBox::accepted, [&dialog]{
        dialog.close();
    });
    dialog.exec();

    m_fields->showSetSubaddressLookaheadPage = check_subaddressLookahead.isChecked();
}