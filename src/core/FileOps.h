#pragma once
#include <QString>
#include <QStringList>
#include <QImage>
#include <QWidget>

// 文件操作:删除(回收站)、重命名、复制/移动、剪贴板、属性
class FileOps {
public:
    // 按"归档"页选项删除(是否确认、是否进回收站);全部成功返回 true,失败写 error;用户取消返回 false 且 error 为空
    static bool deleteFiles(QWidget* parent, const QStringList& paths, QString* error);
    // 弹出重命名对话框并改名;成功返回 true 并写 newPath;用户取消返回 false 且 error 为空
    static bool renameWithDialog(QWidget* parent, const QString& path, QString* newPath, QString* error);
    // 直接改名(不弹窗)
    static bool renameTo(const QString& path, const QString& newName, QString* newPath, QString* error);
    // 选择目标文件夹后复制或移动;move 为 true 是移动;按"归档"页处理重复文件
    static bool copyOrMoveWithDialog(QWidget* parent, const QStringList& paths, bool move, QString* error);
    // 复制或移动到指定文件夹(处理重复文件策略);成功返回 true
    // created 非空时,收集实际生成的新路径(粘贴后用来选中副本)
    static bool copyOrMove(QWidget* parent, const QStringList& paths, const QString& destDir, bool move, QString* error,
                           QStringList* created = nullptr);
    // 在目标文件夹里生成不重复的文件名(用 RenameSeparator,如 a_1.jpg)
    static QString uniqueName(const QString& destDir, const QString& fileName);
    // 两个文件内容是否完全相同
    static bool isIdentical(const QString& a, const QString& b);
    // 把图片复制到剪贴板
    static void copyImageToClipboard(const QImage& image);
    // 把文件剪切到剪贴板(在资源管理器粘贴时是移动)
    static void cutFilesToClipboard(const QStringList& paths);
    // 把文件复制到剪贴板(可在资源管理器粘贴)
    static void copyFilesToClipboard(const QStringList& paths);
    // 显示文件属性对话框(名称、路径、大小、时间、图片尺寸)
    static void showProperties(QWidget* parent, const QString& path);
    // 显示一个错误对话框(标题和内容都来自界面文字表)
    static void showError(QWidget* parent, const QString& message);
};
