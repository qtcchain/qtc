// Copyright (c) 2011-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_PSBTOPERATIONSDIALOG_H
#define BITCOIN_QT_PSBTOPERATIONSDIALOG_H

#include <QDialog>
#include <QString>

#include <psbt.h>
#include <qt/clientmodel.h>
#include <qt/walletmodel.h>

namespace Ui {
class PSBTOperationsDialog;
}

/** Dialog showing transaction details. */
class PSBTOperationsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit PSBTOperationsDialog(QWidget* parent, WalletModel* walletModel, ClientModel* clientModel);
    ~PSBTOperationsDialog();

    void openWithPSBT(PartiallySignedTransaction psbt_tx);

public Q_SLOTS:
    void signTransaction();
    void broadcastTransaction();
    void copyToClipboard();
    void saveTransaction();

private:
    Ui::PSBTOperationsDialog* m_ui;
    PartiallySignedTransaction m_transaction_data;
    WalletModel* m_wallet_model;
    ClientModel* m_client_model;

    enum class StatusLevel {
        INFO,
        WARN,
        ERR
    };

    size_t couldSignInputs(const PartiallySignedTransaction &psbt_tx);
    bool slhdsaFips205ForNextBlock() const;
    void updateTransactionDisplay();
    QString renderTransaction(const PartiallySignedTransaction &psbt_tx);
    void showStatus(const QString &msg, StatusLevel level);
    void showTransactionStatus(const PartiallySignedTransaction &psbt_tx);
};

#endif // BITCOIN_QT_PSBTOPERATIONSDIALOG_H
