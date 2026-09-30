# mt2

轻量级文件管理 CLI 工具，面向 Termux / Android。

## 编译

```
clang++ -Os -s -o mt2 mt2_v3.cpp
```

支持 ZIP（需安装 libzip）：

```
pkg install libzip
clang++ -Os -s -o mt2 mt2_v3.cpp -lzip
```

## 命令

`copy move delete list read write chmod stat grep rename replace replacedir hexdump hexwrite diff wait zip unzip`

示例：

```
mt2 copy /sdcard/a.txt /sdcard/b.txt
mt2 delete /sdcard/tmp
mt2 grep /sdcard/log.txt error
mt2 replacedir /sdcard/src foo bar .txt -r
```

全局选项：`--lang en|zh`、`--force`、`--follow-symlinks`、`--fail-on-symlinks`、`--dry-run`。

## 文件

| 文件 | 说明 |
|------|------|
| mt2_v2.cpp | v2 主程序 |
| mt2_v3.cpp | v3 主程序（当前） |
| mt2.h | C API 头文件 |
| mt2_so_wrapper.cpp | 编译为 .so 共享库 |
| Mt2.java / mt2_jni.c | Android JNI 桥接 |
| mt2_sh.cpp | Shizuku 自动安装辅助 |

## 集成

共享库：`dlopen("libmt2.so")` 调用 `mt2_exec(argc, argv)`。
