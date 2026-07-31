// SPDX-License-Identifier: BSD-3-Clause
// SPDX-FileCopyrightText: The Monero Project

#ifndef FEATHER_PAGEHARDWAREDEVICE_H
#define FEATHER_PAGEHARDWAREDEVICE_H

#include <QCheckBox>
#include <QWizardPage>

class WizardFields;

namespace Ui {
    class PageHardwareDevice;
}

class PageHardwareDevice : public QWizardPage
{
Q_OBJECT

public:
    explicit PageHardwareDevice(WizardFields *fields, QWidget *parent = nullptr);
    void initializePage() override;
    bool validatePage() override;
    int nextId() const override;
    bool isComplete() const override;

private:
    void onOptionsClicked();
    void onDeviceTypeChanged();

    Ui::PageHardwareDevice *ui;
    WizardFields *m_fields;
    // Only meaningful for a Trezor Safe 7, so it is hidden for other devices.
    QCheckBox *m_checkBluetooth = nullptr;
};


#endif //FEATHER_PAGEHARDWAREDEVICE_H
