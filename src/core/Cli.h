#pragma once
#include <QString>

// 命令行小工具(给 MoonManager 用):--list-formats / --check-config / --list-commands
namespace Cli {

// 是否是本模块处理的命令行选项
bool isToolOption(const QString& arg);
// 执行小工具,返回进程退出码(0=成功)。需要 QApplication 已创建。
int runTool(const QString& option);
// 把一段文字(UTF-8,自动加换行)写到标准输出/标准错误;没有可写的输出时什么也不做
void write(const QString& text, bool toStdErr);

} // namespace Cli
