#include "artwork_settings.h"
#include "rawg_client.h"
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace xmb {
bool configureArtwork(QWidget* parent, const QString& status) {
    QDialog dialog(parent);
    dialog.setWindowTitle("Game Artwork");
    auto* layout = new QVBoxLayout(&dialog);
    auto* description = new QLabel("Imported games without artwork are matched by title against the public Steam store automatically (no key needed, cached on this PC). Add a RAWG API key to also fill installed Steam games and other titles. Your own artwork always takes priority.");
    description->setWordWrap(true);
    layout->addWidget(description);
    auto* label = new QLabel("RAWG API &key (optional)");
    auto* key = new QLineEdit;
    label->setBuddy(key);
    key->setEchoMode(QLineEdit::Password);
    key->setMaxLength(128);
    const bool environmentKey = RawgClient::hasEnvironmentKey();
    key->setPlaceholderText(environmentKey ? "Configured through RAWG_API_KEY" :
                            RawgClient::hasApiKey() ? "Configured • enter a replacement if needed" : "Enter your RAWG API key");
    key->setEnabled(!environmentKey);
    layout->addWidget(label);
    layout->addWidget(key);
    auto* source = new QLabel("<a href=\"https://rawg.io/apidocs\">Artwork from RAWG · Get an API key</a>");
    source->setOpenExternalLinks(true);
    layout->addWidget(source);
    if (!status.isEmpty()) {
        auto* lastRun = new QLabel(status);
        lastRun->setTextFormat(Qt::PlainText);
        lastRun->setWordWrap(true);
        layout->addWidget(lastRun);
    }
    auto* error = new QLabel;
    error->setWordWrap(true);
    layout->addWidget(error);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    auto* fetch = buttons->addButton("Fetch Artwork", QDialogButtonBox::AcceptRole);
    fetch->setDefault(true);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        QString message;
        if (!key->text().isEmpty() && !RawgClient::saveApiKey(key->text().trimmed(), message)) {
            error->setText(message);
            return;
        }
        dialog.accept();
    });
    return dialog.exec() == QDialog::Accepted;
}
}
