

#include <iostream>
#include <string>
#include <vector>
#include <cstdlib>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <array>


static std::string exec_cmd(const std::string& cmd) {
    std::array<char, 128> buffer;
    std::string result;
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
    if (!pipe) {
        return "";
    }
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
        result += buffer.data();
    }
    return result;
}


static bool check_adb_available() {
    std::string output = exec_cmd("which adb 2>/dev/null");
    return !output.empty();
}


static bool check_device_connected() {
    std::string output = exec_cmd("adb devices 2>&1");

    size_t pos = output.find("device");
    if (pos == std::string::npos) return false;


    if (output.find("device") != std::string::npos) {

        size_t line_start = output.rfind('\n', pos);
        if (line_start != std::string::npos) {
            std::string line = output.substr(line_start + 1);
            if (line.find("device") != std::string::npos &&
                line.find("unauthorized") == std::string::npos &&
                line.find("offline") == std::string::npos) {
                return true;
            }
        }
    }
    return false;
}


static bool install_shizuku() {

    const std::string apk_src = "/sdcard/Download/shizuku.apk";
    std::string check_cmd = "test -f " + apk_src + " && echo exists";
    std::string result = exec_cmd(check_cmd.c_str());
    if (result.find("exists") == std::string::npos) {
        std::cerr << "❌ 未找到 Shizuku APK，请先下载到 /sdcard/Download/shizuku.apk" << std::endl;
        std::cerr << "   官方下载: https://shizuku.rikka.app/download/" << std::endl;
        return false;
    }

    std::cout << "[*] 正在推送 Shizuku APK..." << std::endl;
    std::string push_cmd = "adb push " + apk_src + " /data/local/tmp/ 2>&1";
    std::string push_out = exec_cmd(push_cmd.c_str());
    if (push_out.find("error") != std::string::npos) {
        std::cerr << "❌ 推送失败: " << push_out << std::endl;
        return false;
    }

    std::cout << "[*] 正在安装 Shizuku..." << std::endl;
    std::string install_cmd = "adb install -r -g /data/local/tmp/shizuku.apk 2>&1";
    std::string install_out = exec_cmd(install_cmd.c_str());
    if (install_out.find("Success") == std::string::npos) {
        std::cerr << "❌ 安装失败: " << install_out << std::endl;
        return false;
    }

    return true;
}


static bool start_shizuku() {
    std::cout << "[*] 正在启动 Shizuku 服务..." << std::endl;

    std::string start_cmd = "adb shell am start -n moe.shizuku.privileged.api/.StartActivity 2>&1";
    std::string out = exec_cmd(start_cmd.c_str());

    if (out.find("Error") != std::string::npos || out.find("Exception") != std::string::npos) {
        std::cerr << "❌ 启动失败: " << out << std::endl;
        return false;
    }
    return true;
}


static void print_help() {
    std::cout << "用法: mt2 sh <子命令>" << std::endl;
    std::cout << "子命令:" << std::endl;
    std::cout << "  adb        通过 ADB 安装并激活 Shizuku" << std::endl;
    std::cout << "  help       显示此帮助信息" << std::endl;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_help();
        return 1;
    }

    std::string subcmd = argv[1];
    if (subcmd == "help" || subcmd == "--help" || subcmd == "-h") {
        print_help();
        return 0;
    }

    if (subcmd != "adb") {
        std::cerr << "未知子命令: " << subcmd << std::endl;
        print_help();
        return 1;
    }


    std::cout << "[*] 正在检查 ADB..." << std::endl;
    if (!check_adb_available()) {
        std::cerr << "❌ ADB 未安装，请先运行: pkg install android-tools" << std::endl;
        return 1;
    }

    std::cout << "[*] 正在检测设备连接..." << std::endl;
    if (!check_device_connected()) {
        std::cerr << "❌ 未检测到已授权的设备，请确保:" << std::endl;
        std::cerr << "  1. 手机已开启 USB 调试" << std::endl;
        std::cerr << "  2. 手机已连接 USB 并在手机上允许 USB 调试" << std::endl;
        std::cerr << "  3. 执行 'adb devices' 查看设备状态" << std::endl;
        return 1;
    }

    if (!install_shizuku()) {
        std::cerr << "❌ Shizuku 安装失败" << std::endl;
        return 1;
    }

    if (!start_shizuku()) {
        std::cerr << "❌ Shizuku 启动失败，请手动打开 Shizuku 应用授权" << std::endl;
        return 1;
    }

    std::cout << "✅ Shizuku 已成功安装并启动！请在手机上确认授权。" << std::endl;
    std::cout << "   - 打开 Shizuku 应用，查看状态是否为 '已运行'。" << std::endl;
    std::cout << "   - 如果未自动授权，点击 '启动' 或 '授权' 完成。" << std::endl;
    return 0;
}
