#include <QtWidgets>
#include <QCryptographicHash>
#include <QDataStream>
#include <QFileInfo>
#include <QSaveFile>
#include <QPointer>

#include "AES.h"
#include "lz4.h"
#include "zstd.h"

enum class Compression : quint8 { None, Zip, Lz4, Zstd };

struct Asset {
    QString type;
    QString slot;
    QString fileName;
    QByteArray sha256;
    QByteArray data;
};

struct Identity {
    QString name;
    QString description;
    QStringList tones;
    QMap<QString, Asset> settingImages;
    QList<Asset> generalImages;
    QList<Asset> detailImages;
    QList<Asset> refAudio;
    QList<Asset> refVideo;
    QList<Asset> privateLora;
};

struct Character {
    QString name = QStringLiteral("未命名角色");
    QString description;
    QString author = QStringLiteral("本人");
    QString version = QStringLiteral("1.0.0");
    QString license = QStringLiteral("仅供个人使用");
    QList<Identity> identities;
};

static const QList<QPair<QString, QString>> kSettingImageSlots = {
    {"Portrait", "肖像"}, {"Front", "正面"}, {"Left", "左侧"},
    {"Right", "右侧"}, {"Rear", "背面"}, {"Top", "顶部"},
    {"Bottom", "底部"}, {"LeftFronthalf", "左前半身"},
    {"RightFronthalf", "右前半身"}, {"LeftRearHalf", "左后半身"},
    {"RightRearHalf", "右后半身"}
};

static QByteArray compressData(const QByteArray &input, Compression mode) {
    if (mode == Compression::None) return input;
    if (mode == Compression::Zip || mode == Compression::Lz4) return qCompress(input, 9);

    const size_t bound = ZSTD_compressBound(input.size());
    QByteArray output(static_cast<int>(bound), Qt::Uninitialized);
    const size_t size = ZSTD_compress(output.data(), bound, input.constData(), input.size(), 5);
    if (ZSTD_isError(size)) return {};
    output.resize(static_cast<int>(size));
    return output;
}

static QByteArray decompressData(const QByteArray &input, Compression mode, qint64 originalSize) {
    if (mode == Compression::None) return input;
    if (mode == Compression::Zip || mode == Compression::Lz4) return qUncompress(input);
    if (originalSize < 0 || originalSize > INT_MAX) return {};

    QByteArray output(static_cast<int>(originalSize), Qt::Uninitialized);
    const size_t size = ZSTD_decompress(output.data(), originalSize, input.constData(), input.size());
    if (ZSTD_isError(size)) return {};
    output.resize(static_cast<int>(size));
    return output;
}

static QByteArray crypt(const QByteArray &input, const QString &password, bool encrypt) {
    const QByteArray key = QCryptographicHash::hash(password.toUtf8(), QCryptographicHash::Sha256);
    const QByteArray iv(16, '\0');
    AES aes(AESKeyLength::AES_256);

    if (encrypt) {
        const int padding = 16 - (input.size() % 16);
        QByteArray padded = input;
        padded.append(QByteArray(padding, static_cast<char>(padding)));
        unsigned char *encrypted = aes.EncryptCBC(
            reinterpret_cast<const unsigned char *>(padded.constData()), padded.size(),
            reinterpret_cast<const unsigned char *>(key.constData()),
            reinterpret_cast<const unsigned char *>(iv.constData()));
        QByteArray result(reinterpret_cast<const char *>(encrypted), padded.size());
        delete[] encrypted;
        return result;
    }

    if (input.isEmpty() || input.size() % 16 != 0) return {};
    unsigned char *decrypted = aes.DecryptCBC(
        reinterpret_cast<const unsigned char *>(input.constData()), input.size(),
        reinterpret_cast<const unsigned char *>(key.constData()),
        reinterpret_cast<const unsigned char *>(iv.constData()));
    QByteArray result(reinterpret_cast<const char *>(decrypted), input.size());
    delete[] decrypted;

    const unsigned char padding = static_cast<unsigned char>(result.back());
    if (padding == 0 || padding > 16 || result.right(padding) != QByteArray(padding, static_cast<char>(padding))) return {};
    result.chop(padding);
    return result;
}

class MainWindow final : public QMainWindow {
    Q_OBJECT
public:
    MainWindow() {
        buildUi();
        newCharacter();
    }

private:
    Character character;
    int currentIdentity = 0;
    QString currentPath;

    QLineEdit *nameEdit = nullptr;
    QLineEdit *authorEdit = nullptr;
    QLineEdit *versionEdit = nullptr;
    QLineEdit *licenseEdit = nullptr;
    QTextEdit *descriptionEdit = nullptr;
    QLineEdit *identityNameEdit = nullptr;
    QTextEdit *identityDescriptionEdit = nullptr;
    QListWidget *identityList = nullptr;
    QListWidget *assetList = nullptr;
    QComboBox *compressionBox = nullptr;
    QCheckBox *aesBox = nullptr;
    QLineEdit *passwordEdit = nullptr;
    QLabel *statusLabel = nullptr;
    QMap<QString, QPushButton *> settingImageButtons;
    QTimer previewTimer;
    QLabel *previewPopup = nullptr;
    QByteArray previewData;

    void buildUi() {
        setWindowTitle("CharacterAssetStudio");
        resize(1200, 780);
        setMinimumSize(980, 650);
        setStyleSheet(R"(
            QMainWindow { background: #f4f7fb; }
            QGroupBox { font-weight: 600; border: 1px solid #dbe3ef; border-radius: 10px; margin-top: 12px; padding: 12px; background: white; }
            QGroupBox::title { left: 12px; padding: 0 5px; color: #25344d; }
            QLineEdit, QTextEdit, QComboBox, QListWidget { border: 1px solid #d5deea; border-radius: 7px; padding: 7px; background: #fbfcfe; }
            QPushButton { border: 0; border-radius: 7px; padding: 9px 15px; background: #e8eef8; color: #243653; }
            QPushButton:hover { background: #d9e5f6; }
            QPushButton#primary { background: #2867d8; color: white; font-weight: 600; }
            QPushButton#settingImage { min-height: 72px; background: #f6f9fe; border: 1px dashed #aec3e6; color: #405574; }
            QPushButton#settingImage[assigned="true"] { background: #eaf3ff; border: 1px solid #6394e6; color: #15458d; font-weight: 600; }
            QLabel#title { font-size: 24px; font-weight: 700; color: #1f2d44; }
            QLabel#muted { color: #71809a; }
        )");

        auto *central = new QWidget;
        setCentralWidget(central);
        previewPopup = new QLabel(nullptr, Qt::ToolTip | Qt::FramelessWindowHint);
        previewPopup->setAttribute(Qt::WA_ShowWithoutActivating);
        previewPopup->setAlignment(Qt::AlignCenter);
        previewPopup->setStyleSheet("QLabel{background:#ffffff;border:1px solid #8da7cc;border-radius:8px;padding:6px;}");
        previewPopup->hide();
        previewTimer.setSingleShot(true);
        previewTimer.setInterval(500);
        connect(&previewTimer, &QTimer::timeout, this, &MainWindow::showPendingPreview);
        qApp->installEventFilter(this);
        auto *root = new QVBoxLayout(central);
        root->setContentsMargins(28, 22, 28, 22);
        root->setSpacing(16);

        auto *header = new QHBoxLayout;
        auto *title = new QLabel("角色资产容器");
        title->setObjectName("title");
        header->addWidget(title);
        auto *subtitle = new QLabel("个人虚拟形象与模特资源管理");
        subtitle->setObjectName("muted");
        header->addWidget(subtitle);
        header->addStretch();
        auto *open = new QPushButton("打开容器");
        auto *saveAs = new QPushButton("另存为");
        auto *save = new QPushButton("保存");
        save->setObjectName("primary");
        header->addWidget(open);
        header->addWidget(saveAs);
        header->addWidget(save);
        root->addLayout(header);

        connect(open, &QPushButton::clicked, this, &MainWindow::openFile);
        connect(save, &QPushButton::clicked, this, &MainWindow::saveFile);
        connect(saveAs, &QPushButton::clicked, this, &MainWindow::saveAsFile);

        auto *tabs = new QTabWidget;
        tabs->addTab(characterTab(), "角色信息");
        tabs->addTab(identityTab(), "身份与资源");
        tabs->addTab(settingsTab(), "容器设置");
        root->addWidget(tabs, 1);

        statusLabel = new QLabel("准备就绪");
        statusLabel->setObjectName("muted");
        root->addWidget(statusLabel);
    }

    QWidget *characterTab() {
        auto *widget = new QWidget;
        auto *layout = new QVBoxLayout(widget);
        auto *box = new QGroupBox("基本信息");
        auto *form = new QFormLayout(box);
        nameEdit = new QLineEdit;
        authorEdit = new QLineEdit;
        versionEdit = new QLineEdit;
        licenseEdit = new QLineEdit;
        descriptionEdit = new QTextEdit;
        descriptionEdit->setFixedHeight(170);
        form->addRow("角色名称", nameEdit);
        form->addRow("作者", authorEdit);
        form->addRow("版本", versionEdit);
        form->addRow("许可", licenseEdit);
        form->addRow("总体描述", descriptionEdit);
        layout->addWidget(box);
        layout->addStretch();

        connect(nameEdit, &QLineEdit::textChanged, this, [this](const QString &value) { character.name = value; });
        connect(authorEdit, &QLineEdit::textChanged, this, [this](const QString &value) { character.author = value; });
        connect(versionEdit, &QLineEdit::textChanged, this, [this](const QString &value) { character.version = value; });
        connect(licenseEdit, &QLineEdit::textChanged, this, [this](const QString &value) { character.license = value; });
        connect(descriptionEdit, &QTextEdit::textChanged, this, [this] { character.description = descriptionEdit->toPlainText(); });
        return widget;
    }

    QWidget *identityTab() {
        auto *widget = new QWidget;
        auto *layout = new QHBoxLayout(widget);
        auto *left = new QGroupBox("身份列表");
        auto *leftLayout = new QVBoxLayout(left);
        identityList = new QListWidget;
        auto *addIdentity = new QPushButton("＋ 新建身份");
        auto *removeIdentity = new QPushButton("删除身份");
        leftLayout->addWidget(identityList);
        leftLayout->addWidget(addIdentity);
        leftLayout->addWidget(removeIdentity);
        layout->addWidget(left, 1);

        auto *scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        auto *content = new QWidget;
        auto *rightLayout = new QVBoxLayout(content);

        auto *detailBox = new QGroupBox("身份详情");
        auto *detailForm = new QFormLayout(detailBox);
        identityNameEdit = new QLineEdit;
        identityDescriptionEdit = new QTextEdit;
        identityDescriptionEdit->setFixedHeight(86);
        detailForm->addRow("身份名称", identityNameEdit);
        detailForm->addRow("私有描述", identityDescriptionEdit);
        rightLayout->addWidget(detailBox);

        auto *settingBox = new QGroupBox("标准参考图 · settingImage");
        auto *settingLayout = new QGridLayout(settingBox);
        settingLayout->setHorizontalSpacing(10);
        settingLayout->setVerticalSpacing(10);
        for (int index = 0; index < kSettingImageSlots.size(); ++index) {
            const auto &[key, title] = kSettingImageSlots[index];
            auto *button = new QPushButton(title + "\n点击导入");
            button->setObjectName("settingImage");
            button->setProperty("settingSlot", key);
            button->setMouseTracking(true);
            button->installEventFilter(this);
            button->setProperty("assigned", false);
            button->setToolTip(key + " · 点击导入或替换");
            settingLayout->addWidget(button, index / 4, index % 4);
            settingImageButtons.insert(key, button);
            connect(button, &QPushButton::clicked, this, [this, key] { importSettingImage(key); });
        }
        rightLayout->addWidget(settingBox);

        auto *assetBox = new QGroupBox("补充资源");
        auto *assetLayout = new QVBoxLayout(assetBox);
        auto *assetNote = new QLabel("通用参考图与细节参考图不占用固定机位；音频、视频和 Lora 同样随容器保存。");
        assetNote->setObjectName("muted");
        assetLayout->addWidget(assetNote);
        assetList = new QListWidget;
        assetList->setMouseTracking(true);
        assetList->viewport()->setMouseTracking(true);
        assetList->viewport()->installEventFilter(this);
        assetLayout->addWidget(assetList, 1);
        auto *buttons = new QHBoxLayout;
        auto *generalImage = new QPushButton("导入通用参考图");
        auto *detailImage = new QPushButton("导入细节参考图");
        auto *audio = new QPushButton("导入音频");
        auto *video = new QPushButton("导入视频");
        auto *lora = new QPushButton("导入 Lora");
        auto *removeAsset = new QPushButton("移除资源");
        buttons->addWidget(generalImage);
        buttons->addWidget(detailImage);
        buttons->addWidget(audio);
        buttons->addWidget(video);
        buttons->addWidget(lora);
        buttons->addWidget(removeAsset);
        assetLayout->addLayout(buttons);
        rightLayout->addWidget(assetBox, 1);

        scroll->setWidget(content);
        layout->addWidget(scroll, 3);

        connect(addIdentity, &QPushButton::clicked, this, [this] {
            Identity identity;
            identity.name = QString("身份 %1").arg(character.identities.size() + 1);
            character.identities.append(identity);
            refreshIdentities();
            identityList->setCurrentRow(character.identities.size() - 1);
        });
        connect(removeIdentity, &QPushButton::clicked, this, [this] {
            if (character.identities.size() <= 1) return;
            character.identities.removeAt(currentIdentity);
            currentIdentity = 0;
            refreshIdentities();
        });
        connect(identityList, &QListWidget::currentRowChanged, this, &MainWindow::selectIdentity);
        connect(identityNameEdit, &QLineEdit::textChanged, this, [this](const QString &value) {
            if (!hasCurrentIdentity()) return;
            character.identities[currentIdentity].name = value;
            if (identityList->currentItem()) identityList->currentItem()->setText(value);
        });
        connect(identityDescriptionEdit, &QTextEdit::textChanged, this, [this] {
            if (hasCurrentIdentity()) character.identities[currentIdentity].description = identityDescriptionEdit->toPlainText();
        });
        connect(generalImage, &QPushButton::clicked, this, [this] { importAssets("图片 (*.png *.jpg *.jpeg *.webp *.bmp)", "通用参考图"); });
        connect(detailImage, &QPushButton::clicked, this, [this] { importAssets("图片 (*.png *.jpg *.jpeg *.webp *.bmp)", "细节参考图"); });
        connect(audio, &QPushButton::clicked, this, [this] { importAssets("音频 (*.wav *.mp3 *.ogg *.flac)", "音频"); });
        connect(video, &QPushButton::clicked, this, [this] { importAssets("视频 (*.mp4 *.mov *.webm *.avi)", "视频"); });
        connect(lora, &QPushButton::clicked, this, [this] { importAssets("Lora (*.safetensors *.pt *.ckpt *.bin)", "Lora"); });
        connect(removeAsset, &QPushButton::clicked, this, [this] { removeSelectedAsset(); });
        return widget;
    }

    QWidget *settingsTab() {
        auto *widget = new QWidget;
        auto *layout = new QVBoxLayout(widget);
        auto *box = new QGroupBox("打包选项");
        auto *form = new QFormLayout(box);
        compressionBox = new QComboBox;
        compressionBox->addItems({"不压缩", "Zip（Qt Deflate）", "LZ4（真实 LZ4）", "Zstandard（高压缩比）"});
        aesBox = new QCheckBox("启用 AES-256-CBC 加密");
        passwordEdit = new QLineEdit;
        passwordEdit->setEchoMode(QLineEdit::Password);
        passwordEdit->setPlaceholderText("保存时输入密码");
        form->addRow("压缩方式", compressionBox);
        form->addRow("加密", aesBox);
        form->addRow("密码", passwordEdit);
        layout->addWidget(box);
        auto *hint = new QLabel("容器会保存角色元数据、每个身份的 settingImage 标准机位图及其补充资源。\nAES 开启后请妥善保管密码，程序不会保存密码。");
        hint->setObjectName("muted");
        layout->addWidget(hint);
        layout->addStretch();
        return widget;
    }

    bool eventFilter(QObject *watched, QEvent *event) override {
        if (event->type() == QEvent::MouseMove) {
            const auto *mouseEvent = static_cast<QMouseEvent *>(event);
            if (watched == assetList->viewport()) {
                const QModelIndex index = assetList->indexAt(mouseEvent->pos());
                if (!index.isValid()) clearPreview();
                else {
                    const QString category = index.data(Qt::UserRole).toString();
                    const int itemIndex = index.data(Qt::UserRole + 1).toInt();
                    const Asset *asset = findAsset(category, itemIndex);
                    if (asset && (category == "通用参考图" || category == "细节参考图")) beginPreview(asset->data);
                    else clearPreview();
                }
            } else {
                auto *button = qobject_cast<QPushButton *>(watched);
                if (button && button->objectName() == "settingImage") {
                    const QString slot = button->property("settingSlot").toString();
                    if (hasCurrentIdentity() && character.identities[currentIdentity].settingImages.contains(slot)) beginPreview(character.identities[currentIdentity].settingImages.value(slot).data);
                    else clearPreview();
                }
            }
        } else if (event->type() == QEvent::Leave) clearPreview();
        return QMainWindow::eventFilter(watched, event);
    }

    const Asset *findAsset(const QString &category, int index) const {
        if (!hasCurrentIdentity()) return nullptr;
        const auto &identity = character.identities[currentIdentity];
        const QList<Asset> *assets = category == "通用参考图" ? &identity.generalImages : category == "细节参考图" ? &identity.detailImages : nullptr;
        if (!assets || index < 0 || index >= assets->size()) return nullptr;
        return &assets->at(index);
    }

    void beginPreview(const QByteArray &data) {
        if (data.isEmpty()) { clearPreview(); return; }
        if (previewData == data && (previewTimer.isActive() || previewPopup->isVisible())) return;
        previewData = data;
        previewPopup->hide();
        previewTimer.start();
    }

    void clearPreview() {
        previewTimer.stop();
        previewData.clear();
        previewPopup->hide();
    }

    void showPendingPreview() {
        if (previewData.isEmpty()) return;
        QPixmap pixmap;
        if (!pixmap.loadFromData(previewData)) { clearPreview(); return; }
        previewPopup->setPixmap(pixmap.scaled(QSize(420, 320), Qt::KeepAspectRatio, Qt::SmoothTransformation));
        previewPopup->adjustSize();
        const QPoint cursor = QCursor::pos();
        const QScreen *screen = QApplication::screenAt(cursor);
        const QRect bounds = screen ? screen->availableGeometry() : QRect(0, 0, 1280, 800);
        QPoint position = cursor + QPoint(18, 18);
        if (position.x() + previewPopup->width() > bounds.right()) position.setX(cursor.x() - previewPopup->width() - 18);
        if (position.y() + previewPopup->height() > bounds.bottom()) position.setY(cursor.y() - previewPopup->height() - 18);
        previewPopup->move(position);
        previewPopup->show();
    }
    bool hasCurrentIdentity() const {
        return currentIdentity >= 0 && currentIdentity < character.identities.size();
    }

    QString slotTitle(const QString &slot) const {
        for (const auto &[key, title] : kSettingImageSlots) if (key == slot) return title;
        return slot;
    }

    void newCharacter() {
        character = Character{};
        character.identities.append({"默认身份", {}, {}, {}, {}, {}, {}, {}, {}});
        currentIdentity = 0;
        populateCharacterFields();
        refreshIdentities();
    }

    void populateCharacterFields() {
        const QSignalBlocker blockName(nameEdit);
        const QSignalBlocker blockAuthor(authorEdit);
        const QSignalBlocker blockVersion(versionEdit);
        const QSignalBlocker blockLicense(licenseEdit);
        const QSignalBlocker blockDescription(descriptionEdit);
        nameEdit->setText(character.name);
        authorEdit->setText(character.author);
        versionEdit->setText(character.version);
        licenseEdit->setText(character.license);
        descriptionEdit->setPlainText(character.description);
    }

    void refreshIdentities() {
        const QSignalBlocker blocker(identityList);
        identityList->clear();
        for (const auto &identity : character.identities) identityList->addItem(identity.name);
        if (!character.identities.isEmpty()) {
            currentIdentity = qBound(0, currentIdentity, character.identities.size() - 1);
            identityList->setCurrentRow(currentIdentity);
            selectIdentity(currentIdentity);
        }
    }

    void selectIdentity(int row) {
        if (row < 0 || row >= character.identities.size()) return;
        currentIdentity = row;
        const auto &identity = character.identities[row];
        const QSignalBlocker blockName(identityNameEdit);
        const QSignalBlocker blockDescription(identityDescriptionEdit);
        identityNameEdit->setText(identity.name);
        identityDescriptionEdit->setPlainText(identity.description);
        refreshSettingImages();
        refreshAssets();
    }

    void refreshSettingImages() {
        if (!hasCurrentIdentity()) return;
        const auto &images = character.identities[currentIdentity].settingImages;
        for (const auto &[slot, title] : kSettingImageSlots) {
            auto *button = settingImageButtons.value(slot);
            const bool assigned = images.contains(slot);
            button->setProperty("assigned", assigned);
            button->setText(assigned ? title + "\n" + images.value(slot).fileName : title + "\n点击导入");
            button->setToolTip(assigned ? slot + " · " + images.value(slot).fileName + " · 点击替换" : slot + " · 点击导入");
            button->style()->unpolish(button);
            button->style()->polish(button);
        }
    }

    void refreshAssets() {
        assetList->clear();
        if (!hasCurrentIdentity()) return;
        auto append = [this](const QList<Asset> &assets, const QString &category) {
            for (int index = 0; index < assets.size(); ++index) {
                const auto &asset = assets[index];
                auto *item = new QListWidgetItem(QString("[%1]  %2  ·  %3 KB  · SHA %4")
                    .arg(category, asset.fileName)
                    .arg(asset.data.size() / 1024.0, 0, 'f', 1)
                    .arg(QString::fromLatin1(asset.sha256.toHex().left(12))));
                item->setData(Qt::UserRole, category);
                item->setData(Qt::UserRole + 1, index);
                assetList->addItem(item);
            }
        };
        const auto &identity = character.identities[currentIdentity];
        append(identity.generalImages, "通用参考图");
        append(identity.detailImages, "细节参考图");
        append(identity.refAudio, "音频");
        append(identity.refVideo, "视频");
        append(identity.privateLora, "Lora");
    }

    void removeSelectedAsset() {
        if (!hasCurrentIdentity() || !assetList->currentItem()) return;
        const auto *item = assetList->currentItem();
        const QString category = item->data(Qt::UserRole).toString();
        const int index = item->data(Qt::UserRole + 1).toInt();
        auto &identity = character.identities[currentIdentity];
        QList<Asset> *assets = nullptr;
        if (category == "通用参考图") assets = &identity.generalImages;
        else if (category == "细节参考图") assets = &identity.detailImages;
        else if (category == "音频") assets = &identity.refAudio;
        else if (category == "视频") assets = &identity.refVideo;
        else if (category == "Lora") assets = &identity.privateLora;
        if (assets && index >= 0 && index < assets->size()) assets->removeAt(index);
        refreshAssets();
    }

    void importSettingImage(const QString &slot) {
        if (!hasCurrentIdentity()) return;
        const QString path = QFileDialog::getOpenFileName(this, "设置 " + slotTitle(slot) + " 参考图", QString(), "图片 (*.png *.jpg *.jpeg *.webp *.bmp)");
        if (path.isEmpty()) return;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            QMessageBox::warning(this, "无法导入", "无法读取所选图片。");
            return;
        }
        const QByteArray data = file.readAll();
        character.identities[currentIdentity].settingImages.insert(slot, {"标准参考图", slot, QFileInfo(path).fileName(), QCryptographicHash::hash(data, QCryptographicHash::Sha256), data});
        refreshSettingImages();
        statusLabel->setText("已设置 " + slotTitle(slot) + " 参考图");
    }

    void importAssets(const QString &filter, const QString &type) {
        if (!hasCurrentIdentity()) return;
        const QStringList files = QFileDialog::getOpenFileNames(this, "选择" + type, QString(), filter);
        auto &identity = character.identities[currentIdentity];
        for (const QString &path : files) {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly)) continue;
            const QByteArray data = file.readAll();
            const Asset asset{type, QFileInfo(path).baseName(), QFileInfo(path).fileName(), QCryptographicHash::hash(data, QCryptographicHash::Sha256), data};
            if (type == "通用参考图") identity.generalImages.append(asset);
            else if (type == "细节参考图") identity.detailImages.append(asset);
            else if (type == "音频") identity.refAudio.append(asset);
            else if (type == "视频") identity.refVideo.append(asset);
            else if (type == "Lora") identity.privateLora.append(asset);
        }
        refreshAssets();
        statusLabel->setText(QString("已导入 %1 个%2资源，并生成 SHA-256 校验值").arg(files.size()).arg(type));
    }

    static void writeAsset(QDataStream &stream, const Asset &asset) {
        stream << asset.type << asset.slot << asset.fileName << asset.sha256 << asset.data;
    }

    static bool readAsset(QDataStream &stream, Asset &asset, bool hasHash) {
        stream >> asset.type >> asset.slot >> asset.fileName;
        if (hasHash) stream >> asset.sha256;
        stream >> asset.data;
        if (stream.status() != QDataStream::Ok) return false;
        const QByteArray calculated = QCryptographicHash::hash(asset.data, QCryptographicHash::Sha256);
        if (hasHash && asset.sha256 != calculated) return false;
        asset.sha256 = calculated;
        return true;
    }

    static void appendLegacyAsset(Identity &identity, const Asset &asset) {
        if (asset.type == "通用参考图") identity.generalImages.append(asset);
        else if (asset.type == "细节参考图") identity.detailImages.append(asset);
        else if (asset.type == "音频") identity.refAudio.append(asset);
        else if (asset.type == "视频") identity.refVideo.append(asset);
        else if (asset.type == "Lora") identity.privateLora.append(asset);
    }

    QByteArray serialize() const {
        QByteArray raw;
        QDataStream stream(&raw, QIODevice::WriteOnly);
        stream.setVersion(QDataStream::Qt_5_15);
        stream << character.name << character.description << character.author << character.version << character.license;
        stream << quint32(character.identities.size());
        for (const auto &identity : character.identities) {
            stream << identity.name << identity.description << identity.tones;
            stream << quint32(identity.settingImages.size());
            for (auto it = identity.settingImages.cbegin(); it != identity.settingImages.cend(); ++it) {
                stream << it.key();
                writeAsset(stream, it.value());
            }
            const QList<QList<Asset>> groups = {identity.generalImages, identity.detailImages, identity.refAudio, identity.refVideo, identity.privateLora};
            for (const auto &assets : groups) {
                stream << quint32(assets.size());
                for (const auto &asset : assets) writeAsset(stream, asset);
            }
        }
        return raw;
    }

    bool deserialize(const QByteArray &raw, quint16 containerVersion) {
        QDataStream stream(raw);
        stream.setVersion(QDataStream::Qt_5_15);
        quint32 identityCount = 0;
        stream >> character.name >> character.description >> character.author >> character.version >> character.license >> identityCount;
        if (stream.status() != QDataStream::Ok || identityCount > 1000) return false;
        character.identities.clear();
        for (quint32 i = 0; i < identityCount; ++i) {
            Identity identity;
            stream >> identity.name >> identity.description >> identity.tones;
            if (containerVersion >= 2) {
                quint32 imageCount = 0;
                stream >> imageCount;
                if (imageCount > static_cast<quint32>(kSettingImageSlots.size())) return false;
                for (quint32 imageIndex = 0; imageIndex < imageCount; ++imageIndex) {
                    QString key;
                    Asset asset;
                    stream >> key;
                    if (!readAsset(stream, asset, containerVersion >= 3)) return false;
                    identity.settingImages.insert(key, asset);
                }
            }
            if (containerVersion >= 3) {
                QList<QList<Asset> *> groups = {&identity.generalImages, &identity.detailImages, &identity.refAudio, &identity.refVideo, &identity.privateLora};
                for (auto *assets : groups) {
                    quint32 count = 0;
                    stream >> count;
                    if (count > 100000) return false;
                    for (quint32 assetIndex = 0; assetIndex < count; ++assetIndex) {
                        Asset asset;
                        if (!readAsset(stream, asset, true)) return false;
                        assets->append(asset);
                    }
                }
            } else {
                quint32 count = 0;
                stream >> count;
                if (count > 100000) return false;
                for (quint32 assetIndex = 0; assetIndex < count; ++assetIndex) {
                    Asset asset;
                    if (!readAsset(stream, asset, false)) return false;
                    appendLegacyAsset(identity, asset);
                }
            }
            character.identities.append(identity);
        }
        return stream.status() == QDataStream::Ok;
    }

    bool writeContainer(const QString &path) {
        const QByteArray raw = serialize();
        const Compression compression = static_cast<Compression>(compressionBox->currentIndex());
        QByteArray packed = compressData(raw, compression);
        if (packed.isEmpty() && !raw.isEmpty()) return false;
        const bool encrypted = aesBox->isChecked();
        if (encrypted) {
            if (passwordEdit->text().isEmpty()) {
                QMessageBox::warning(this, "需要密码", "启用 AES 后请输入密码。");
                return false;
            }
            packed = crypt(packed, passwordEdit->text(), true);
        }

        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly)) return false;
        QDataStream stream(&file);
        stream.setVersion(QDataStream::Qt_5_15);
        stream.writeRawData("CASC", 4);
        stream << quint16(3) << quint8(compression) << quint8(encrypted ? 1 : 0) << qint64(raw.size()) << packed;
        if (!file.commit()) return false;
        currentPath = path;
        statusLabel->setText("已保存：" + QFileInfo(path).fileName());
        return true;
    }

    void saveFile() {
        if (currentPath.isEmpty()) saveAsFile();
        else if (!writeContainer(currentPath)) QMessageBox::warning(this, "保存失败", "无法写入容器文件。");
    }

    void saveAsFile() {
        const QString path = QFileDialog::getSaveFileName(this, "保存角色资产容器", character.name + ".casc", "Character Asset Container (*.casc)");
        if (!path.isEmpty() && !writeContainer(path)) QMessageBox::warning(this, "保存失败", "无法写入容器文件。");
    }

    void openFile() {
        const QString path = QFileDialog::getOpenFileName(this, "打开角色资产容器", QString(), "Character Asset Container (*.casc)");
        if (path.isEmpty()) return;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) return;
        QDataStream stream(&file);
        stream.setVersion(QDataStream::Qt_5_15);
        char magic[4];
        quint16 version = 0;
        quint8 compression = 0;
        quint8 encrypted = 0;
        qint64 originalSize = 0;
        QByteArray packed;
        stream.readRawData(magic, 4);
        stream >> version >> compression >> encrypted >> originalSize >> packed;
        if (stream.status() != QDataStream::Ok || memcmp(magic, "CASC", 4) != 0 || (version != 1 && version != 2 && version != 3)) {
            QMessageBox::warning(this, "无法打开", "文件格式不受支持。");
            return;
        }
        if (encrypted) {
            bool accepted = false;
            const QString password = QInputDialog::getText(this, "输入密码", "AES-256 密码", QLineEdit::Password, QString(), &accepted);
            if (!accepted) return;
            packed = crypt(packed, password, false);
        }
        const QByteArray raw = decompressData(packed, static_cast<Compression>(compression), originalSize);
        if (raw.isEmpty() && originalSize != 0) {
            QMessageBox::warning(this, "无法打开", "容器内容损坏或密码错误。");
            return;
        }
        if (!deserialize(raw, version)) {
            QMessageBox::warning(this, "无法打开", "容器内容损坏或密码错误。");
            return;
        }
        currentPath = path;
        currentIdentity = 0;
        populateCharacterFields();
        refreshIdentities();
        statusLabel->setText("已打开：" + QFileInfo(path).fileName());
    }
};

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setApplicationName("CharacterAssetStudio");
    app.setStyle("Fusion");
    MainWindow window;
    window.show();
    return app.exec();
}

#include "main.moc"