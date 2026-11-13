#include "FileOps.h"
#include "Config.h"
#include "Strings.h"
#include "ImageLoader.h"

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QRegularExpression>
#include <QUrl>

// 按"归档"页选项删除
bool FileOps::deleteFiles(QWidget* parent, const QStringList& paths, QString* error)
{
    if (error != nullptr) error->clear();
    if (paths.isEmpty()) return false;
    const QString mode = Config::instance().getEnum(QStringLiteral("Archive/DeleteMode"));
    const bool permanent = (mode == QLatin1String("Permanent"));
    if (mode != QLatin1String("NoConfirm")) {
        QString text;
        if (paths.size() == 1) {
            text = S(permanent ? "dlg.deleteConfirmPermanent.one" : "dlg.deleteConfirm.one")
                       .arg(QFileInfo(paths.first()).fileName());
        } else {
            text = S(permanent ? "dlg.deleteConfirmPermanent.many" : "dlg.deleteConfirm.many")
                       .arg(paths.size());
        }
        const QMessageBox::StandardButton b = QMessageBox::question(
            parent, S("dlg.deleteConfirm.title"), text, QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (b != QMessageBox::Yes) return false;
    }
    for (int i = 0; i < paths.size(); ++i) {
        const QString p = paths.at(i);
        bool ok = false;
        if (permanent) {
            ok = QFile::remove(p);
        } else {
            ok = QFile::moveToTrash(p);
        }
        if (!ok) {
            if (error != nullptr) {
                *error = S("err.delete").arg(p).arg(permanent ? QFile(p).errorString()
                                                               : S("err.trashFailed"));
            }
            return false;
        }
    }
    return true;
}

// 弹出重命名对话框并改名
bool FileOps::renameWithDialog(QWidget* parent, const QString& path, QString* newPath, QString* error)
{
    if (error != nullptr) error->clear();
    const QFileInfo fi(path);
    bool accepted = false;
    const QString name = QInputDialog::getText(parent, S("dlg.rename.title"), S("dlg.rename.label"),
                                               QLineEdit::Normal, fi.fileName(), &accepted);
    if (!accepted) return false;
    if (name == fi.fileName()) return false;
    return renameTo(path, name, newPath, error);
}

// 直接改名
bool FileOps::renameTo(const QString& path, const QString& newName, QString* newPath, QString* error)
{
    const QFileInfo fi(path);
    const QString name = newName.trimmed();
    static const QRegularExpression bad(QStringLiteral("[\\\\/:*?\"<>|]"));
    if (name.isEmpty() || name == QLatin1String(".") || name == QLatin1String("..")
        || bad.match(name).hasMatch()) {
        if (error != nullptr) *error = S("err.invalidName").arg(newName);
        return false;
    }
    const QString dest = fi.absolutePath() + QLatin1Char('/') + name;
    const bool sameIgnoringCase = (dest.compare(fi.absoluteFilePath(), Qt::CaseInsensitive) == 0);
    if (!sameIgnoringCase && QFileInfo::exists(dest)) {
        if (error != nullptr) *error = S("err.exists").arg(name);
        return false;
    }
    QFile f(path);
    if (!f.rename(dest)) {
        if (error != nullptr) *error = S("err.rename").arg(path).arg(f.errorString());
        return false;
    }
    if (newPath != nullptr) *newPath = dest;
    return true;
}

// 选择目标文件夹后复制或移动
bool FileOps::copyOrMoveWithDialog(QWidget* parent, const QStringList& paths, bool move, QString* error)
{
    if (error != nullptr) error->clear();
    if (paths.isEmpty()) return false;
    const QString dir = QFileDialog::getExistingDirectory(
        parent, S(move ? "dlg.chooseFolder.move" : "dlg.chooseFolder.copy"),
        QFileInfo(paths.first()).absolutePath());
    if (dir.isEmpty()) return false;
    return copyOrMove(parent, paths, dir, move, error);
}

// 复制或移动到指定文件夹
bool FileOps::copyOrMove(QWidget* parent, const QStringList& paths, const QString& destDir, bool move, QString* error)
{
    if (error != nullptr) error->clear();
    Config& cfg = Config::instance();
    const QString policy = cfg.getEnum(QStringLiteral("Archive/DuplicateAction"));
    const bool skipIdentical = cfg.getBool(QStringLiteral("Archive/SkipIfIdentical"));
    if (!QFileInfo(destDir).isDir()) {
        if (error != nullptr) *error = S("err.folderNotFound").arg(destDir);
        return false;
    }
    for (int i = 0; i < paths.size(); ++i) {
        const QString src = paths.at(i);
        const QString fileName = QFileInfo(src).fileName();
        QString dest = QDir(destDir).absoluteFilePath(fileName);
        const bool samePlace = (QFileInfo(src).absoluteFilePath().compare(
                                    QFileInfo(dest).absoluteFilePath(), Qt::CaseInsensitive) == 0);
        if (samePlace) {
            if (move) continue;  // 移动到原位置:什么也不用做
            dest = QDir(destDir).absoluteFilePath(uniqueName(destDir, fileName));
        } else if (QFileInfo::exists(dest)) {
            if (skipIdentical && isIdentical(src, dest)) {
                if (move && !QFile::remove(src)) {
                    if (error != nullptr) *error = S("err.move").arg(src).arg(destDir).arg(QFile(src).errorString());
                    return false;
                }
                continue;
            }
            QString action = policy;
            if (action == QLatin1String("Ask")) {
                QMessageBox box(QMessageBox::Question, S("dlg.duplicate.title"),
                                S("dlg.duplicate.text").arg(fileName), QMessageBox::NoButton, parent);
                QPushButton* bOver = box.addButton(S("btn.overwrite"), QMessageBox::AcceptRole);
                QPushButton* bSkip = box.addButton(S("btn.skip"), QMessageBox::RejectRole);
                QPushButton* bRen = box.addButton(S("btn.autoRename"), QMessageBox::ActionRole);
                box.setEscapeButton(bSkip);
                box.setDefaultButton(bSkip);
                box.exec();
                if (box.clickedButton() == bOver) action = QStringLiteral("Overwrite");
                else if (box.clickedButton() == bRen) action = QStringLiteral("AutoRename");
                else action = QStringLiteral("Skip");
            }
            if (action == QLatin1String("Skip")) {
                continue;
            } else if (action == QLatin1String("AutoRename")) {
                dest = QDir(destDir).absoluteFilePath(uniqueName(destDir, fileName));
            } else {  // Overwrite
                if (!QFile::remove(dest)) {
                    if (error != nullptr) *error = S(move ? "err.move" : "err.copy").arg(src).arg(destDir).arg(QFile(dest).errorString());
                    return false;
                }
            }
        }
        if (move) {
            QFile f(src);
            if (!f.rename(dest)) {
                // 跨磁盘时 rename 会失败:改为复制后删除原文件
                QFile c(src);
                if (!c.copy(dest)) {
                    if (error != nullptr) *error = S("err.move").arg(src).arg(destDir).arg(c.errorString());
                    return false;
                }
                if (!QFile::remove(src)) {
                    if (error != nullptr) *error = S("err.move").arg(src).arg(destDir).arg(QFile(src).errorString());
                    return false;
                }
            }
        } else {
            QFile c(src);
            if (!c.copy(dest)) {
                if (error != nullptr) *error = S("err.copy").arg(src).arg(destDir).arg(c.errorString());
                return false;
            }
        }
    }
    return true;
}

// 在目标文件夹里生成不重复的文件名:a.jpg -> a_1.jpg -> a_2.jpg
QString FileOps::uniqueName(const QString& destDir, const QString& fileName)
{
    if (!QFileInfo::exists(QDir(destDir).absoluteFilePath(fileName))) return fileName;
    const QString sep = Config::instance().getString(QStringLiteral("Archive/RenameSeparator"));
    const QFileInfo fi(fileName);
    const QString base = fi.completeBaseName();
    const QString suffix = fi.suffix();
    for (int n = 1; n < 100000; ++n) {
        QString candidate = base + sep + QString::number(n);
        if (!suffix.isEmpty()) candidate += QLatin1Char('.') + suffix;
        if (!QFileInfo::exists(QDir(destDir).absoluteFilePath(candidate))) return candidate;
    }
    Config::fatal(S("err.exists").arg(fileName));
    return fileName;
}

// 两个文件内容是否完全相同
bool FileOps::isIdentical(const QString& a, const QString& b)
{
    QFileInfo fa(a);
    QFileInfo fb(b);
    if (fa.size() != fb.size()) return false;
    QFile f1(a);
    QFile f2(b);
    if (!f1.open(QIODevice::ReadOnly) || !f2.open(QIODevice::ReadOnly)) return false;
    const qint64 chunk = 1 << 16;
    while (!f1.atEnd()) {
        if (f1.read(chunk) != f2.read(chunk)) return false;
    }
    return true;
}

// 图片复制到剪贴板
void FileOps::copyImageToClipboard(const QImage& image)
{
    QApplication::clipboard()->setImage(image);
}

// 文件剪切到剪贴板:在复制的基础上加 Preferred DropEffect=2(粘贴时是移动)
void FileOps::cutFilesToClipboard(const QStringList& paths)
{
    QMimeData* mime = new QMimeData();
    QList<QUrl> urls;
    for (int i = 0; i < paths.size(); ++i) urls << QUrl::fromLocalFile(paths.at(i));
    mime->setUrls(urls);
    QByteArray effect(4, '\0');
    effect[0] = 2;
    mime->setData(QStringLiteral("Preferred DropEffect"), effect);
    QApplication::clipboard()->setMimeData(mime);
}

// 文件复制到剪贴板(资源管理器可粘贴)
void FileOps::copyFilesToClipboard(const QStringList& paths)
{
    QMimeData* mime = new QMimeData();
    QList<QUrl> urls;
    for (int i = 0; i < paths.size(); ++i) urls << QUrl::fromLocalFile(paths.at(i));
    mime->setUrls(urls);
    QApplication::clipboard()->setMimeData(mime);
}

// 属性对话框
void FileOps::showProperties(QWidget* parent, const QString& path)
{
    const QFileInfo fi(path);
    QStringList lines;
    lines << S("dlg.properties.name").arg(fi.fileName());
    lines << S("dlg.properties.path").arg(QDir::toNativeSeparators(fi.absolutePath()));
    lines << S("dlg.properties.size").arg(QLocale().toString(fi.size()));
    lines << S("dlg.properties.modified").arg(fi.lastModified().toString(QStringLiteral("yyyy/M/d hh:mm:ss")));
    ImageInfo info;
    QString err;
    if (ImageLoader::instance().readInfo(path, &info, &err) && info.valid) {
        lines << S("dlg.properties.image").arg(imageInfoLine(info, true));
    } else {
        lines << S("dlg.properties.image").arg(err);  // 读不出图片信息:直接把原因显示出来
    }
    QMessageBox::information(parent, S("dlg.properties.title"), lines.join(QStringLiteral("\n")));
}

// 错误对话框
void FileOps::showError(QWidget* parent, const QString& message)
{
    QMessageBox::critical(parent, S("dlg.error.title"), message);
}
