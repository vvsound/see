#include "ExtensionsDialog.h"
#include "core/Config.h"
#include "core/Strings.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QDialogButtonBox>
#include <QRegularExpression>
#include <QSet>

// 构造:列表 + 输入框 + 添加/删除 + 确定/取消
ExtensionsDialog::ExtensionsDialog(QWidget* parent) : QDialog(parent)
{
    setWindowTitle(S("dlg.extensions.title"));
    resize(360, 420);

    QVBoxLayout* root = new QVBoxLayout(this);
    QLabel* hint = new QLabel(S("dlg.extensions.hint"), this);
    root->addWidget(hint);

    list_ = new QListWidget(this);
    list_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    const QStringList cur = Config::instance().getList(QStringLiteral("Files/Extensions"));
    for (int i = 0; i < cur.size(); ++i) {
        const QString e = normalize(cur.at(i));
        if (e.isEmpty()) {
            Config::fatal(S("err.badExtension").arg(cur.at(i)));
        }
        list_->addItem(e);
    }
    root->addWidget(list_, 1);

    QHBoxLayout* row = new QHBoxLayout();
    edit_ = new QLineEdit(this);
    addButton_ = new QPushButton(S("btn.add"), this);
    addButton_->setEnabled(false);
    removeButton_ = new QPushButton(S("btn.remove"), this);
    row->addWidget(edit_, 1);
    row->addWidget(addButton_);
    row->addWidget(removeButton_);
    root->addLayout(row);

    QLabel* note = new QLabel(S("dlg.extensions.note"), this);
    note->setWordWrap(true);
    root->addWidget(note);

    QDialogButtonBox* box = new QDialogButtonBox(this);
    QPushButton* ok = box->addButton(S("btn.ok"), QDialogButtonBox::AcceptRole);
    QPushButton* cancel = box->addButton(S("btn.cancel"), QDialogButtonBox::RejectRole);
    root->addWidget(box);

    connect(edit_, &QLineEdit::textChanged, this, &ExtensionsDialog::onTextChanged);
    connect(edit_, &QLineEdit::returnPressed, this, &ExtensionsDialog::onAdd);
    connect(addButton_, &QPushButton::clicked, this, &ExtensionsDialog::onAdd);
    connect(removeButton_, &QPushButton::clicked, this, &ExtensionsDialog::onRemove);
    connect(ok, &QPushButton::clicked, this, &ExtensionsDialog::onAccept);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
}

// 对话框里当前的扩展名列表
QStringList ExtensionsDialog::extensions() const
{
    QStringList out;
    QSet<QString> seen;
    for (int i = 0; i < list_->count(); ++i) {
        const QString e = list_->item(i)->text();
        if (!seen.contains(e)) {
            seen.insert(e);
            out << e;
        }
    }
    return out;
}

// 添加输入框里的扩展名(非法直接报错,重复则只选中已有项)
void ExtensionsDialog::onAdd()
{
    const QString raw = edit_->text();
    if (raw.trimmed().isEmpty()) return;
    const QString e = normalize(raw);
    if (e.isEmpty()) {
        QMessageBox::warning(this, S("err.title"), S("err.badExtension").arg(raw));
        return;
    }
    const QList<QListWidgetItem*> found = list_->findItems(e, Qt::MatchExactly);
    if (found.isEmpty()) {
        list_->addItem(e);
        list_->setCurrentRow(list_->count() - 1);
    } else {
        list_->setCurrentItem(found.first());
    }
    edit_->clear();
    edit_->setFocus();
}

// 删除选中项
void ExtensionsDialog::onRemove()
{
    const QList<QListWidgetItem*> sel = list_->selectedItems();
    for (int i = 0; i < sel.size(); ++i) {
        delete list_->takeItem(list_->row(sel.at(i)));
    }
}

// 确定:写入 Files/Extensions;列表为空直接报错不保存
void ExtensionsDialog::onAccept()
{
    const QStringList exts = extensions();
    if (exts.isEmpty()) {
        QMessageBox::warning(this, S("err.title"), S("err.extEmpty"));
        return;
    }
    Config::instance().setList(QStringLiteral("Files/Extensions"), exts);
    accept();
}

// 输入框有内容才允许点添加
void ExtensionsDialog::onTextChanged(const QString& text)
{
    addButton_->setEnabled(!text.trimmed().isEmpty());
}

// 规范化:去前导点/星号、去空白、小写;只允许字母数字
QString ExtensionsDialog::normalize(const QString& text)
{
    QString t = text.trimmed().toLower();
    while (t.startsWith(QLatin1Char('.')) || t.startsWith(QLatin1Char('*'))) {
        t.remove(0, 1);
    }
    static const QRegularExpression re(QStringLiteral("^[a-z0-9]+$"));
    if (!re.match(t).hasMatch()) return QString();
    return t;
}
