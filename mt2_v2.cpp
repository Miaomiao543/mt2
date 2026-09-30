

#define HAS_LIBZIP

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <deque>
#include <stack>
#include <unordered_set>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <iomanip>
#include <csignal>
#include <cerrno>
#include <climits>
#include <functional>

#ifdef HAS_LIBZIP
#include <zip.h>
#endif


enum class Lang { EN, ZH };
static Lang g_lang = Lang::ZH;

struct I18n {
    std::string en;
    std::string zh;
    const std::string& get() const { return g_lang == Lang::EN ? en : zh; }
};

#define I(en_str, zh_str) I18n{en_str, zh_str}.get()


static volatile sig_atomic_t g_interrupted = 0;
static void signal_handler(int ) {
    g_interrupted = 1;
    std::cerr << I("\nInterrupted. Cleaning up...\n", "\n操作已中断，正在清理...\n");
}

static void install_signal_handlers() {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
}

static void cleanup_and_exit() {
    std::cerr << I("Operation aborted.\n", "操作已中止。\n");
    std::exit(130);
}


#define CHECK_INTERRUPT() do { if (g_interrupted) { cleanup_and_exit(); return false; } } while(0)
#define CHECK_INTERRUPT_VOID() do { if (g_interrupted) { cleanup_and_exit(); return; } } while(0)
#define CHECK_INTERRUPT_VOID_ONLY() do { if (g_interrupted) { cleanup_and_exit(); } } while(0)


static const std::vector<std::string> PROTECTED_PATHS = {
    "/", "/data", "/system", "/vendor", "/product", "/sdcard", "/storage"
};

static bool is_protected_path(const std::string& path) {
    std::string normalized = path;

    while (normalized.length() > 1 && normalized.back() == '/')
        normalized.pop_back();

    for (const auto& p : PROTECTED_PATHS) {
        if (normalized == p) return true;
    }
    return false;
}

static bool check_protected(const std::string& path, const std::string& operation) {
    if (is_protected_path(path)) {
        std::cerr << I("ERROR: Operation '", "错误：操作 '")
                  << operation
                  << I("' on protected path '", "' 在受保护路径 '")
                  << path
                  << I("' is not allowed. Use --force to override (DANGEROUS).\n",
                       "' 上被禁止。使用 --force 强制覆盖（危险）。\n");
        return false;
    }
    return true;
}


static bool is_path_safe(const std::string& path) {

    if (!path.empty() && path[0] == '/' && path.find("..") == std::string::npos)
        return true;


    if (path.find("..") != std::string::npos) {
        std::cerr << I("ERROR: Path traversal detected: ", "错误：检测到路径遍历：")
                  << path << std::endl;
        return false;
    }
    return true;
}

static std::string sanitize_zip_path(const std::string& entry) {

    std::string result = entry;

    while (!result.empty() && result[0] == '/')
        result = result.substr(1);

    if (result.find("..") != std::string::npos) {
        std::cerr << I("WARNING: Skipping unsafe ZIP entry: ",
                       "警告：跳过不安全的 ZIP 条目：")
                  << entry << std::endl;
        return "";
    }
    return result;
}


enum class SymlinkPolicy { FOLLOW, SKIP, FAIL };
static SymlinkPolicy g_symlink_policy = SymlinkPolicy::SKIP;

static bool is_symlink(const std::string& path) {
    struct stat st;
    return lstat(path.c_str(), &st) == 0 && S_ISLNK(st.st_mode);
}

static bool handle_symlink(const std::string& path, const std::string& operation) {
    if (!is_symlink(path)) return true;

    switch (g_symlink_policy) {
        case SymlinkPolicy::SKIP:
            return false;
        case SymlinkPolicy::FOLLOW:
            return true;
        case SymlinkPolicy::FAIL:
            std::cerr << I("ERROR: Symbolic link encountered during ",
                           "错误：在操作 ")
                      << operation << I(": ", " 中遇到符号链接：")
                      << path << std::endl;
            return false;
    }
    return false;
}


static bool mkdir_p(const std::string& path, mode_t mode = 0755) {
    if (path.empty()) return false;

    std::string current;
    size_t start = 0;


    if (path[0] == '/') {
        current = "/";
        start = 1;
    }

    while (start < path.length()) {
        size_t slash = path.find('/', start);
        if (slash == std::string::npos) slash = path.length();

        current += path.substr(start, slash - start);
        if (!current.empty()) {
            struct stat st;
            if (stat(current.c_str(), &st) != 0) {
                if (mkdir(current.c_str(), mode) != 0 && errno != EEXIST) {
                    std::cerr << I("Failed to create directory: ",
                                   "创建目录失败：")
                              << current << " (" << strerror(errno) << ")" << std::endl;
                    return false;
                }
            } else if (!S_ISDIR(st.st_mode)) {
                std::cerr << I("Path exists but is not a directory: ",
                               "路径存在但不是目录：")
                          << current << std::endl;
                return false;
            }
        }
        current += "/";
        start = slash + 1;
    }
    return true;
}


static bool copy_file_raw(const char* src, const char* dst) {
    CHECK_INTERRUPT();


    std::string dst_str(dst);
    size_t last_slash = dst_str.find_last_of('/');
    if (last_slash != std::string::npos && last_slash > 0) {
        std::string parent = dst_str.substr(0, last_slash);
        struct stat st;
        if (stat(parent.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
            std::cerr << I("Destination parent directory does not exist: ",
                           "目标父目录不存在：")
                      << parent << std::endl;
            return false;
        }
    }

    int fd_in = open(src, O_RDONLY);
    if (fd_in < 0) {
        std::cerr << I("Cannot open source: ", "无法打开源文件：")
                  << src << " (" << strerror(errno) << ")" << std::endl;
        return false;
    }

    int fd_out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd_out < 0) {
        close(fd_in);
        std::cerr << I("Cannot create destination: ", "无法创建目标文件：")
                  << dst << " (" << strerror(errno) << ")" << std::endl;
        return false;
    }

    char buf[65536];
    ssize_t n;
    bool success = true;
    while ((n = read(fd_in, buf, sizeof(buf))) > 0) {
        CHECK_INTERRUPT_VOID_ONLY();
        if (write(fd_out, buf, n) != n) {
            std::cerr << I("Write error: ", "写入错误：") << strerror(errno) << std::endl;
            success = false;
            break;
        }
    }

    close(fd_in);
    close(fd_out);

    if (!success) {
        unlink(dst);
    }
    return success && n == 0;
}


static bool move_file_raw(const char* src, const char* dst) {
    CHECK_INTERRUPT();


    if (rename(src, dst) == 0) return true;


    if (errno == EXDEV) {
        std::cerr << I("Cross-device move, falling back to copy+delete...\n",
                       "跨文件系统移动，回退到复制+删除...\n");
        if (copy_file_raw(src, dst)) {
            return unlink(src) == 0;
        }
    }
    return false;
}


static int delete_file_raw(const char* path, bool check_only = false, bool force = false) {
    CHECK_INTERRUPT();

    std::string path_str(path);
    if (!force && !check_protected(path_str, "delete")) {
        return -1;
    }

    if (!handle_symlink(path_str, "delete")) {

        return 0;
    }

    struct stat st;
    if (lstat(path, &st) != 0) {
        std::cerr << I("Cannot access: ", "无法访问：")
                  << path << " (" << strerror(errno) << ")" << std::endl;
        return -1;
    }

    if (S_ISDIR(st.st_mode)) {

        std::stack<std::pair<std::string, DIR*>> dir_stack;
        std::vector<std::string> dirs_to_remove;

        DIR* d = opendir(path);
        if (!d) {
            std::cerr << I("Cannot open directory: ", "无法打开目录：")
                      << path << " (" << strerror(errno) << ")" << std::endl;
            return -1;
        }

        dir_stack.push({path_str, d});

        while (!dir_stack.empty()) {
            CHECK_INTERRUPT();

            auto& [current_dir, current_dp] = dir_stack.top();
            dirent* e = readdir(current_dp);

            if (e == nullptr) {

                closedir(current_dp);
                dirs_to_remove.push_back(current_dir);
                dir_stack.pop();
                continue;
            }

            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
                continue;

            std::string full = current_dir + "/" + e->d_name;

            if (is_symlink(full)) {

                continue;
            }

            struct stat entry_st;
            if (lstat(full.c_str(), &entry_st) != 0) continue;

            if (S_ISDIR(entry_st.st_mode)) {
                DIR* subdir = opendir(full.c_str());
                if (subdir) {
                    dir_stack.push({full, subdir});
                }
            } else {
                if (!check_only && unlink(full.c_str()) != 0) {
                    std::cerr << I("Failed to delete: ", "删除失败：")
                              << full << " (" << strerror(errno) << ")" << std::endl;
                }
            }
        }


        for (auto it = dirs_to_remove.rbegin(); it != dirs_to_remove.rend(); ++it) {
            if (!check_only && rmdir(it->c_str()) != 0) {
                std::cerr << I("Failed to remove directory: ", "删除目录失败：")
                          << *it << " (" << strerror(errno) << ")" << std::endl;
            }
        }
        return 0;
    }


    if (check_only) {
        return (access(path, W_OK) == 0) ? 0 : -1;
    }
    return unlink(path) == 0 ? 0 : -1;
}

static std::vector<std::string> list_dir_raw(const char* path) {
    std::vector<std::string> files;
    DIR* d = opendir(path);
    if (!d) return files;
    dirent* e;
    while ((e = readdir(d))) {
        if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0)
            files.push_back(e->d_name);
    }
    closedir(d);
    return files;
}


static bool process_text_stream(const char* path,
    const std::function<bool(const std::string& line)>& processor) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;

    std::string line;
    while (std::getline(in, line)) {
        CHECK_INTERRUPT();
        if (!processor(line)) return false;
    }
    return true;
}

static std::string read_text_raw(const char* path, size_t max_size = 10 * 1024 * 1024) {
    struct stat st;
    if (stat(path, &st) == 0 && st.st_size > (off_t)max_size) {
        std::cerr << I("File too large (", "文件过大（")
                  << st.st_size << I(" bytes), use streaming mode.\n",
                                     " 字节），请使用流式模式。\n");
        return "";
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) return "";
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static bool write_text_raw(const char* path, const std::string& content) {
    CHECK_INTERRUPT();
    std::string tmp_path = std::string(path) + ".mt2tmp";
    std::ofstream out(tmp_path, std::ios::binary);
    if (!out) return false;
    out << content;
    if (!out.good()) {
        unlink(tmp_path.c_str());
        return false;
    }
    out.close();
    CHECK_INTERRUPT();
    if (rename(tmp_path.c_str(), path) != 0) {
        unlink(tmp_path.c_str());
        return false;
    }
    return true;
}


static std::string stat_raw(const char* path) {
    struct stat st;
    if (lstat(path, &st) != 0) return "";
    std::ostringstream info;
    info << I("size:", "大小:") << st.st_size
         << I(" mtime:", " 修改时间:") << st.st_mtime
         << I(" mode:", " 权限:") << std::oct << (st.st_mode & 07777) << std::dec
         << I(" isdir:", " 是目录:") << (S_ISDIR(st.st_mode) ? 1 : 0);
    return info.str();
}


static int rename_batch(const char* dir, const std::string& from, const std::string& to,
                        bool dry_run = false) {
    if (from.empty()) {
        std::cerr << I("ERROR: Empty search string in rename.\n",
                       "错误：重命名搜索字符串为空。\n");
        return 0;
    }

    auto files = list_dir_raw(dir);
    int count = 0;

    for (const auto& f : files) {
        CHECK_INTERRUPT();

        std::string old_name = f;
        std::string new_name = f;


        std::vector<size_t> positions;
        size_t pos = 0;
        while ((pos = new_name.find(from, pos)) != std::string::npos) {
            positions.push_back(pos);
            pos += from.length();
        }

        if (positions.empty()) continue;


        for (auto it = positions.rbegin(); it != positions.rend(); ++it) {
            new_name.replace(*it, from.length(), to);
        }

        if (new_name != old_name) {
            std::cout << old_name << " -> " << new_name;
            if (dry_run) {
                std::cout << I(" [dry-run]", " [预览模式]");
            } else {
                std::string src = std::string(dir) + "/" + old_name;
                std::string dst = std::string(dir) + "/" + new_name;
                if (rename(src.c_str(), dst.c_str()) == 0) {
                    count++;
                } else {
                    std::cout << I(" [FAILED: ", " [失败：")
                              << strerror(errno) << "]";
                }
            }
            std::cout << std::endl;
            if (dry_run) count++;
        }
    }
    return count;
}


static bool replace_in_file(const char* path, const std::string& search, const std::string& replace) {
    if (search.empty()) {
        std::cerr << I("ERROR: Empty search string.\n", "错误：搜索字符串为空。\n");
        return false;
    }

    struct stat st;
    if (stat(path, &st) == 0 && st.st_size > 100 * 1024 * 1024) {
        std::cerr << I("File too large for in-memory replace.\n",
                       "文件过大，无法使用内存替换。\n");
        return false;
    }

    std::string content = read_text_raw(path);
    if (content.empty() && errno) return false;


    std::vector<size_t> positions;
    size_t pos = 0;
    while ((pos = content.find(search, pos)) != std::string::npos) {
        positions.push_back(pos);
        pos += search.length();
    }
    if (positions.empty()) return true;


    for (auto it = positions.rbegin(); it != positions.rend(); ++it) {
        CHECK_INTERRUPT();
        content.replace(*it, search.length(), replace);
    }
    return write_text_raw(path, content);
}


static int replace_in_dir(const char* dir, const std::string& search,
                          const std::string& replace, const std::string& ext = "",
                          bool recursive = false, bool dry_run = false) {
    if (search.empty()) {
        std::cerr << I("ERROR: Empty search string.\n", "错误：搜索字符串为空。\n");
        return 0;
    }

    int count = 0;
    std::deque<std::string> dirs_to_process;
    dirs_to_process.push_back(dir);
    std::unordered_set<std::string> visited;

    while (!dirs_to_process.empty()) {
        CHECK_INTERRUPT();

        std::string current_dir = dirs_to_process.front();
        dirs_to_process.pop_front();

        if (visited.count(current_dir)) continue;
        visited.insert(current_dir);

        auto files = list_dir_raw(current_dir.c_str());

        for (const auto& f : files) {
            CHECK_INTERRUPT();

            std::string full = current_dir + "/" + f;


            if (is_symlink(full)) continue;

            struct stat st;
            if (lstat(full.c_str(), &st) != 0) continue;

            if (S_ISDIR(st.st_mode)) {
                if (recursive) {
                    dirs_to_process.push_back(full);
                }
                continue;
            }

            if (!S_ISREG(st.st_mode)) continue;

            if (!ext.empty()) {
                if (f.size() < ext.size() || f.substr(f.size() - ext.size()) != ext)
                    continue;
            }

            if (dry_run) {
                std::string content = read_text_raw(full.c_str());
                if (content.find(search) != std::string::npos) {
                    std::cout << I("[dry-run] Would replace in: ",
                                   "[预览] 将要替换于：")
                              << full << std::endl;
                    count++;
                }
            } else {
                if (replace_in_file(full.c_str(), search, replace)) {
                    std::cout << I("Replaced in: ", "已替换于：")
                              << full << std::endl;
                    count++;
                }
            }
        }
    }
    return count;
}


class FormatGuard {
    std::ostream& os_;
    std::ios::fmtflags flags_;
    char fill_;
public:
    explicit FormatGuard(std::ostream& os) : os_(os), flags_(os.flags()), fill_(os.fill()) {}
    ~FormatGuard() { os_.flags(flags_); os_.fill(fill_); }
};

static void hexdump(const char* path, size_t len = 0, size_t offset = 0) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::cerr << I("Cannot open file\n", "无法打开文件\n");
        return;
    }

    f.seekg(0, f.end);
    size_t file_size = f.tellg();
    f.seekg(0, f.beg);

    if (offset >= file_size) {
        std::cerr << I("Offset beyond file size\n", "偏移量超出文件大小\n");
        return;
    }

    f.seekg(offset);
    if (len == 0 || len > file_size - offset)
        len = file_size - offset;


    const size_t MAX_DUMP = 1024 * 1024;
    if (len > MAX_DUMP) {
        std::cout << I("Warning: Limiting output to 1MB\n",
                       "警告：输出限制为 1MB\n");
        len = MAX_DUMP;
    }

    FormatGuard guard(std::cout);

    const size_t BLOCK_SIZE = 4096;
    std::vector<char> buf(BLOCK_SIZE);
    size_t total_read = 0;

    while (total_read < len && f.good()) {
        CHECK_INTERRUPT_VOID();

        size_t to_read = std::min(BLOCK_SIZE, len - total_read);
        f.read(buf.data(), to_read);
        size_t rd = f.gcount();

        for (size_t i = 0; i < rd; i += 16) {
            size_t current_offset = offset + total_read + i;

            std::cout << std::hex << std::setw(8) << std::setfill('0') << current_offset << "  ";

            for (size_t j = 0; j < 16; j++) {
                if (i + j < rd)
                    std::cout << std::hex << std::setw(2) << std::setfill('0')
                              << (unsigned int)(unsigned char)buf[i + j] << " ";
                else
                    std::cout << "   ";
                if (j == 7) std::cout << " ";
            }

            std::cout << " |";
            for (size_t j = 0; j < 16 && i + j < rd; j++) {
                char c = buf[i + j];
                std::cout << (isprint(c) ? c : '.');
            }
            std::cout << "|" << std::endl;
        }

        total_read += rd;
    }
}


static bool hexwrite(const char* path, long offset, const std::string& hex_str) {

    std::string hex = hex_str;
    if (hex.size() >= 2 && hex.substr(0, 2) == "0x") {
        hex = hex.substr(2);
    }


    hex.erase(std::remove_if(hex.begin(), hex.end(), ::isspace), hex.end());

    if (hex.length() % 2 != 0) {
        std::cerr << I("Error: Hex string must have even length\n",
                       "错误：十六进制字符串长度必须为偶数\n");
        return false;
    }
    if (offset < 0) {
        std::cerr << I("Error: Offset must be non-negative\n",
                       "错误：偏移量必须非负\n");
        return false;
    }

    std::vector<unsigned char> bytes;
    for (size_t i = 0; i < hex.length(); i += 2) {
        unsigned int byte;
        if (sscanf(hex.substr(i, 2).c_str(), "%02x", &byte) != 1) {
            std::cerr << I("Error: Invalid hex at position ", "错误：位置 ")
                      << i << I("\n", " 处十六进制无效\n");
            return false;
        }
        bytes.push_back((unsigned char)byte);
    }

    struct stat st;
    bool file_exists = (stat(path, &st) == 0);
    if (file_exists && !S_ISREG(st.st_mode)) {
        std::cerr << I("Error: Not a regular file\n", "错误：不是普通文件\n");
        return false;
    }

    if (file_exists && offset > (long)st.st_size) {
        std::cerr << I("Error: Offset exceeds file size. Use offset <= ",
                       "错误：偏移量超出文件大小。请使用 <= ")
                  << st.st_size << std::endl;
        return false;
    }

    int fd = open(path, O_WRONLY | O_CREAT, 0644);
    if (fd < 0) {
        std::cerr << I("Error opening file: ", "打开文件错误：")
                  << strerror(errno) << std::endl;
        return false;
    }
    if (lseek(fd, offset, SEEK_SET) == -1) {
        std::cerr << I("Error seeking: ", "定位错误：")
                  << strerror(errno) << std::endl;
        close(fd);
        return false;
    }
    if (write(fd, bytes.data(), bytes.size()) != (ssize_t)bytes.size()) {
        std::cerr << I("Error writing: ", "写入错误：")
                  << strerror(errno) << std::endl;
        close(fd);
        return false;
    }
    close(fd);
    return true;
}


struct Edit {
    enum Type { DELETE, INSERT, EQUAL } type;
    std::string text;
};


static std::vector<Edit> simple_diff(const std::vector<std::string>& a,
                                      const std::vector<std::string>& b) {
    size_t n = a.size();
    size_t m = b.size();

    if (n == 0 && m == 0) return {};
    if (n == 0) {
        std::vector<Edit> edits;
        for (const auto& line : b) edits.push_back({Edit::INSERT, line});
        return edits;
    }
    if (m == 0) {
        std::vector<Edit> edits;
        for (const auto& line : a) edits.push_back({Edit::DELETE, line});
        return edits;
    }


    const size_t MAX_DIFF_LINES = 5000;
    if (n > MAX_DIFF_LINES || m > MAX_DIFF_LINES) {

        std::vector<Edit> edits;
        size_t i = 0, j = 0;
        while (i < n && j < m) {
            if (a[i] == b[j]) {
                edits.push_back({Edit::EQUAL, a[i]});
                i++; j++;
            } else {

                bool found = false;
                for (size_t look_ahead = 1; look_ahead <= 3 && !found; look_ahead++) {
                    if (i + look_ahead < n && a[i + look_ahead] == b[j]) {
                        for (size_t k = 0; k < look_ahead; k++)
                            edits.push_back({Edit::DELETE, a[i + k]});
                        i += look_ahead;
                        found = true;
                    } else if (j + look_ahead < m && a[i] == b[j + look_ahead]) {
                        for (size_t k = 0; k < look_ahead; k++)
                            edits.push_back({Edit::INSERT, b[j + k]});
                        j += look_ahead;
                        found = true;
                    }
                }
                if (!found) {
                    edits.push_back({Edit::DELETE, a[i]});
                    edits.push_back({Edit::INSERT, b[j]});
                    i++; j++;
                }
            }
        }
        while (i < n) { edits.push_back({Edit::DELETE, a[i]}); i++; }
        while (j < m) { edits.push_back({Edit::INSERT, b[j]}); j++; }
        return edits;
    }


    std::vector<std::vector<size_t>> dp(n + 1, std::vector<size_t>(m + 1, 0));
    for (size_t i = 1; i <= n; i++) {
        for (size_t j = 1; j <= m; j++) {
            if (a[i-1] == b[j-1])
                dp[i][j] = dp[i-1][j-1] + 1;
            else
                dp[i][j] = std::max(dp[i-1][j], dp[i][j-1]);
        }
    }


    std::vector<Edit> edits;
    size_t i = n, j = m;
    while (i > 0 || j > 0) {
        if (i > 0 && j > 0 && a[i-1] == b[j-1]) {
            edits.push_back({Edit::EQUAL, a[i-1]});
            i--; j--;
        } else if (j > 0 && (i == 0 || dp[i][j-1] >= dp[i-1][j])) {
            edits.push_back({Edit::INSERT, b[j-1]});
            j--;
        } else {
            edits.push_back({Edit::DELETE, a[i-1]});
            i--;
        }
    }
    std::reverse(edits.begin(), edits.end());
    return edits;
}

static void diff_files(const char* file1, const char* file2) {
    std::ifstream f1(file1), f2(file2);
    if (!f1) {
        std::cerr << I("Cannot open file: ", "无法打开文件：") << file1 << std::endl;
        return;
    }
    if (!f2) {
        std::cerr << I("Cannot open file: ", "无法打开文件：") << file2 << std::endl;
        return;
    }


    const size_t MAX_LINES = 100000;
    std::vector<std::string> lines1, lines2;
    std::string line;


    bool is_binary = false;
    while (std::getline(f1, line)) {
        CHECK_INTERRUPT_VOID();
        if (lines1.size() >= MAX_LINES) {
            std::cerr << I("File too large for diff (max ",
                           "文件过大无法比较（最大 ")
                      << MAX_LINES << I(" lines)\n", " 行）\n");
            return;
        }

        if (line.find('\0') != std::string::npos) {
            is_binary = true;
        }
        lines1.push_back(line);
    }

    while (std::getline(f2, line)) {
        CHECK_INTERRUPT_VOID();
        if (lines2.size() >= MAX_LINES) {
            std::cerr << I("File too large for diff (max ",
                           "文件过大无法比较（最大 ")
                      << MAX_LINES << I(" lines)\n", " 行）\n");
            return;
        }
        lines2.push_back(line);
    }

    if (is_binary) {
        std::cerr << I("Binary files differ\n", "二进制文件不同\n");
        return;
    }

    std::cout << "--- " << file1 << std::endl;
    std::cout << "+++ " << file2 << std::endl;

    auto edits = simple_diff(lines1, lines2);

    size_t changes = 0;
    for (const auto& e : edits) {
        if (e.type != Edit::EQUAL) changes++;
    }

    if (changes == 0) {
        std::cout << I("(files are identical)\n", "（文件相同）\n");
        return;
    }


    for (const auto& e : edits) {
        switch (e.type) {
            case Edit::DELETE: std::cout << "- " << e.text << std::endl; break;
            case Edit::INSERT: std::cout << "+ " << e.text << std::endl; break;
            case Edit::EQUAL:  std::cout << "  " << e.text << std::endl; break;
        }
    }
}

#ifdef HAS_LIBZIP

static void add_dir_to_zip(zip_t* archive, const std::string& dir,
                           const std::string& parent_path,
                           std::unordered_set<std::string>& visited) {
    std::deque<std::pair<std::string, std::string>> queue;
    queue.push_back({dir, parent_path});

    while (!queue.empty()) {
        CHECK_INTERRUPT_VOID();

        auto [current_dir, current_parent] = queue.front();
        queue.pop_front();

        if (visited.count(current_dir)) continue;
        visited.insert(current_dir);

        auto files = list_dir_raw(current_dir.c_str());
        for (const auto& f : files) {
            CHECK_INTERRUPT_VOID();

            std::string full = current_dir + "/" + f;


            if (is_symlink(full)) continue;

            std::string zip_name = current_parent.empty() ? f : current_parent + "/" + f;

            struct stat st;
            if (lstat(full.c_str(), &st) != 0) continue;

            if (S_ISDIR(st.st_mode)) {
                zip_dir_add(archive, zip_name.c_str(), ZIP_FL_ENC_GUESS);
                queue.push_back({full, zip_name});
            } else {
                zip_source_t* src = zip_source_file(archive, full.c_str(), 0, 0);
                if (src) {
                    zip_file_add(archive, zip_name.c_str(), src,
                                 ZIP_FL_OVERWRITE | ZIP_FL_ENC_GUESS);
                }
            }
        }
    }
}

static void zip_with_password(const std::string& output,
                              const std::vector<std::string>& inputs,
                              const std::string& password,
                              bool overwrite = false) {
    int err;
    int flags = ZIP_CREATE;
    if (!overwrite) flags |= ZIP_EXCL;

    zip_t* archive = zip_open(output.c_str(), flags, &err);
    if (!archive) {
        std::cerr << I("Failed to create ZIP: ", "创建 ZIP 失败：")
                  << err << std::endl;
        return;
    }

    if (!password.empty())
        zip_set_default_password(archive, password.c_str());

    std::unordered_set<std::string> visited;

    for (const auto& input : inputs) {
        CHECK_INTERRUPT_VOID();

        struct stat st;
        if (lstat(input.c_str(), &st) != 0) {
            std::cerr << I("Skipping ", "跳过 ")
                      << input << " (" << strerror(errno) << ")" << std::endl;
            continue;
        }


        if (is_symlink(input)) continue;

        std::string base_name = input;
        size_t pos = base_name.find_last_of('/');
        if (pos != std::string::npos) base_name = base_name.substr(pos + 1);

        if (S_ISDIR(st.st_mode)) {
            zip_dir_add(archive, base_name.c_str(), ZIP_FL_ENC_GUESS);
            add_dir_to_zip(archive, input, base_name, visited);
        } else {
            zip_source_t* src = zip_source_file(archive, input.c_str(), 0, 0);
            if (src) {
                zip_file_add(archive, base_name.c_str(), src,
                             ZIP_FL_OVERWRITE | ZIP_FL_ENC_GUESS);
            }
        }
    }

    zip_close(archive);
    std::cout << I("ZIP created: ", "ZIP 已创建：") << output << std::endl;
}


static void unzip_with_password(const std::string& zip_path,
                                const std::string& out_dir,
                                const std::string& password,
                                bool overwrite = false) {
    int err;
    zip_t* archive = zip_open(zip_path.c_str(), 0, &err);
    if (!archive) {
        std::cerr << I("Failed to open ZIP: ", "打开 ZIP 失败：")
                  << err << std::endl;
        return;
    }

    if (!password.empty())
        zip_set_default_password(archive, password.c_str());

    mkdir_p(out_dir);

    int n = zip_get_num_entries(archive, 0);
    for (int i = 0; i < n; i++) {
        CHECK_INTERRUPT_VOID();

        const char* name = zip_get_name(archive, i, 0);
        if (!name) continue;


        std::string safe_name = sanitize_zip_path(name);
        if (safe_name.empty()) continue;

        std::string full_out = out_dir + "/" + safe_name;


        struct stat st;
        bool exists = (stat(full_out.c_str(), &st) == 0);
        if (exists && !overwrite) {
            std::cerr << I("Skipping existing file: ", "跳过已存在文件：")
                      << safe_name << std::endl;
            continue;
        }


        if (!safe_name.empty() && safe_name.back() == '/') {
            mkdir_p(full_out);
            continue;
        }


        size_t slash = full_out.find_last_of('/');
        if (slash != std::string::npos) {
            mkdir_p(full_out.substr(0, slash));
        }

        zip_file_t* zf = zip_fopen_index(archive, i, 0);
        if (!zf) {
            std::cerr << I("Failed to open entry: ", "无法打开条目：")
                      << safe_name << std::endl;
            continue;
        }

        std::ofstream out(full_out, std::ios::binary);
        if (!out) {
            std::cerr << I("Failed to create output file: ",
                           "无法创建输出文件：")
                      << full_out << std::endl;
            zip_fclose(zf);
            continue;
        }

        char buf[65536];
        zip_int64_t nread;
        while ((nread = zip_fread(zf, buf, sizeof(buf))) > 0) {
            CHECK_INTERRUPT_VOID_ONLY();
            out.write(buf, nread);
        }

        zip_fclose(zf);
    }

    zip_close(archive);
    std::cout << I("Extracted to: ", "解压到：") << out_dir << std::endl;
}
#endif


struct ParsedArgs {
    std::vector<std::string> positional;
    std::unordered_set<std::string> flags;
    std::unordered_map<std::string, std::string> options;
    bool has_double_dash = false;
};

static ParsedArgs parse_args(const std::vector<std::string>& args) {
    ParsedArgs result;
    bool options_done = false;

    for (size_t i = 0; i < args.size(); i++) {
        if (!options_done && args[i] == "--") {
            options_done = true;
            result.has_double_dash = true;
            continue;
        }

        if (!options_done && args[i].size() > 2 && args[i].substr(0, 2) == "--") {

            size_t eq = args[i].find('=');
            if (eq != std::string::npos) {
                result.options[args[i].substr(0, eq)] = args[i].substr(eq + 1);
            } else {
                result.flags.insert(args[i]);
            }
        } else if (!options_done && args[i].size() == 2 && args[i][0] == '-' &&
                   args[i][1] != '-' && args[i][1] != '\0') {

            std::string opt = args[i];
            std::string key = "-" + std::string(1, opt[1]);
            if (i + 1 < args.size() && args[i + 1][0] != '-') {
                result.options[key] = args[++i];
            } else {
                result.flags.insert(opt);
            }
        } else {
            result.positional.push_back(args[i]);
        }
    }

    return result;
}


struct CmdHelp {
    std::string cmd;
    std::string usage_en;
    std::string usage_zh;
};

static const std::vector<CmdHelp> cmd_helps = {
    {"copy",      "mt2 copy <src> <dst>",
                  "mt2 copy <源文件> <目标文件>"},
    {"move",      "mt2 move <src> <dst>",
                  "mt2 move <源文件> <目标文件>"},
    {"delete",    "mt2 delete <path> [--force]",
                  "mt2 delete <路径> [--force]"},
    {"list",      "mt2 list <dir>",
                  "mt2 list <目录>"},
    {"read",      "mt2 read <file>",
                  "mt2 read <文件>"},
    {"write",     "mt2 write <file> <content>",
                  "mt2 write <文件> <内容>"},
    {"chmod",     "mt2 chmod <path> <mode>",
                  "mt2 chmod <路径> <权限>"},
    {"stat",      "mt2 stat <path>",
                  "mt2 stat <路径>"},
    {"grep",      "mt2 grep <file> <keyword>",
                  "mt2 grep <文件> <关键字>"},
    {"rename",    "mt2 rename <dir> <from> <to> [--dry-run]",
                  "mt2 rename <目录> <原字符串> <新字符串> [--dry-run]"},
    {"replace",   "mt2 replace <file> <search> <replace>",
                  "mt2 replace <文件> <搜索> <替换>"},
    {"replacedir","mt2 replacedir <dir> <search> <replace> [ext] [-r] [--dry-run]",
                  "mt2 replacedir <目录> <搜索> <替换> [扩展名] [-r] [--dry-run]"},
    {"hexdump",   "mt2 hexdump <file> [len] [offset]",
                  "mt2 hexdump <文件> [长度] [偏移量]"},
    {"hexwrite",  "mt2 hexwrite <file> <offset> <hex>",
                  "mt2 hexwrite <文件> <偏移量> <十六进制>"},
    {"diff",      "mt2 diff <file1> <file2>",
                  "mt2 diff <文件1> <文件2>"},
#ifdef HAS_LIBZIP
    {"zip",       "mt2 zip <out.zip> <file/dir...> [-p password] [--overwrite]",
                  "mt2 zip <输出.zip> <文件/目录...> [-p 密码] [--overwrite]"},
    {"unzip",     "mt2 unzip <in.zip> [out_dir] [-p password] [--overwrite]",
                  "mt2 unzip <输入.zip> [输出目录] [-p 密码] [--overwrite]"},
#endif
};

static void print_cmd_usage(const std::string& cmd) {
    for (const auto& h : cmd_helps) {
        if (h.cmd == cmd) {
            std::cerr << I("Usage: ", "用法：")
                      << (g_lang == Lang::EN ? h.usage_en : h.usage_zh) << std::endl;
            return;
        }
    }
    std::cerr << I("Unknown command: ", "未知命令：") << cmd << std::endl;
}

void print_help() {
    if (g_lang == Lang::EN) {
        std::cout << R"(MT2 Tools - Termux File Manager
Usage:
  mt2 <command> [arguments...]

Commands:
  copy    <src> <dst>                        Copy file
  move    <src> <dst>                        Move/rename file
  delete  <path> [--force]                   Delete file or directory
  list    <dir>                              List directory contents
  read    <file>                             Read file content
  write   <file> <content>                   Write content to file
  chmod   <path> <mode>                      Change file permissions
  stat    <path>                             Show file status
  grep    <file> <keyword>                   Search keyword in file
  rename  <dir> <from> <to> [--dry-run]      Batch rename files
  replace <file> <search> <replace>          Search and replace in file
  replacedir <dir> <search> <replace> [ext] [-r] [--dry-run]
                                           Batch replace in directory
  hexdump <file> [len] [offset]              Hex dump viewer (max 1MB)
  hexwrite <file> <offset> <hex>             Write hex at offset
  diff    <file1> <file2>                    Compare two files
)" << std::endl;
#ifdef HAS_LIBZIP
        std::cout << R"(  zip     <out.zip> <file/dir...> [-p pwd] [--overwrite]
                                           Create ZIP
  unzip   <in.zip> [out_dir] [-p pwd] [--overwrite]
                                           Extract ZIP
)" << std::endl;
#endif
        std::cout << R"(
Options:
  --dry-run        Preview changes without modifying files
  -p <password>    Specify password for zip/unzip
  -r               Recursive mode
  --force          Override protected path protection (DANGEROUS)
  --overwrite      Overwrite existing files
  --lang <en|zh>   Set language (default: zh)
  --               Stop option parsing

Environment:
  MT2_LANG=en      Set default language to English
)" << std::endl;
    } else {
        std::cout << R"(MT2 工具 - Termux 文件管理器
用法：
  mt2 <命令> [参数...]

命令：
  copy    <源文件> <目标文件>                复制文件
  move    <源文件> <目标文件>                移动/重命名文件
  delete  <路径> [--force]                   删除文件或目录
  list    <目录>                             列出目录内容
  read    <文件>                             读取文件内容
  write   <文件> <内容>                      写入内容到文件
  chmod   <路径> <权限>                      修改文件权限
  stat    <路径>                             显示文件状态
  grep    <文件> <关键字>                    在文件中搜索关键字
  rename  <目录> <原字符串> <新字符串> [--dry-run]
                                           批量重命名文件
  replace <文件> <搜索> <替换>               在文件中搜索替换
  replacedir <目录> <搜索> <替换> [扩展名] [-r] [--dry-run]
                                           批量目录搜索替换
  hexdump <文件> [长度] [偏移量]             十六进制查看器（最大1MB）
  hexwrite <文件> <偏移量> <十六进制>        在偏移处写入十六进制
  diff    <文件1> <文件2>                    比较两个文件
)" << std::endl;
#ifdef HAS_LIBZIP
        std::cout << R"(  zip     <输出.zip> <文件/目录...> [-p 密码] [--overwrite]
                                           创建 ZIP
  unzip   <输入.zip> [输出目录] [-p 密码] [--overwrite]
                                           解压 ZIP
)" << std::endl;
#endif
        std::cout << R"(
选项：
  --dry-run        预览变更而不修改文件
  -p <密码>        指定 zip/unzip 密码
  -r               递归模式
  --force          覆盖受保护路径保护（危险）
  --overwrite      覆盖已存在文件
  --lang <en|zh>   设置语言（默认：zh）
  --               停止选项解析

环境变量：
  MT2_LANG=en      设置默认语言为英文
)" << std::endl;
    }
}


int main(int argc, char* argv[]) {
    install_signal_handlers();


    const char* env_lang = std::getenv("MT2_LANG");
    if (env_lang && std::string(env_lang) == "en") {
        g_lang = Lang::EN;
    }

    if (argc < 2) {
        print_help();
        return 1;
    }

    std::string cmd = argv[1];


    if (cmd == "--lang" && argc >= 3) {
        std::string lang = argv[2];
        if (lang == "en") g_lang = Lang::EN;
        else if (lang == "zh") g_lang = Lang::ZH;
        else {
            std::cerr << I("Unknown language: ", "未知语言：") << lang << std::endl;
            return 1;
        }
        if (argc < 4) {
            print_help();
            return 0;
        }
        cmd = argv[3];

        std::vector<std::string> args;
        for (int i = 4; i < argc; i++) args.push_back(argv[i]);


        auto parsed = parse_args(args);
        (void)parsed;


        std::cerr << I("Please specify command after language option\n",
                       "请在语言选项后指定命令\n");
        return 1;
    }

    std::vector<std::string> args;
    for (int i = 2; i < argc; i++) args.push_back(argv[i]);

    auto parsed = parse_args(args);
    bool dry_run = parsed.flags.count("--dry-run") > 0;
    bool force = parsed.flags.count("--force") > 0;
    bool recursive = parsed.flags.count("-r") > 0;
    bool overwrite = parsed.flags.count("--overwrite") > 0;
    std::string password = parsed.options.count("-p") ? parsed.options["-p"] : "";


    if (parsed.flags.count("--follow-symlinks")) {
        g_symlink_policy = SymlinkPolicy::FOLLOW;
    } else if (parsed.flags.count("--fail-on-symlinks")) {
        g_symlink_policy = SymlinkPolicy::FAIL;
    }


    if (cmd == "copy") {
        if (parsed.positional.size() != 2) { print_cmd_usage(cmd); return 1; }
        std::cout << (copy_file_raw(parsed.positional[0].c_str(),
                                    parsed.positional[1].c_str()) ? "OK" : "FAIL") << std::endl;
    }
    else if (cmd == "move") {
        if (parsed.positional.size() != 2) { print_cmd_usage(cmd); return 1; }
        std::cout << (move_file_raw(parsed.positional[0].c_str(),
                                    parsed.positional[1].c_str()) ? "OK" : "FAIL") << std::endl;
    }
    else if (cmd == "delete") {
        if (parsed.positional.size() != 1) { print_cmd_usage(cmd); return 1; }
        int ret = delete_file_raw(parsed.positional[0].c_str(), false, force);
        std::cout << (ret == 0 ? "OK" : "FAIL") << std::endl;
    }
    else if (cmd == "list") {
        if (parsed.positional.size() != 1) { print_cmd_usage(cmd); return 1; }
        auto files = list_dir_raw(parsed.positional[0].c_str());
        for (const auto& f : files) std::cout << f << std::endl;
    }
    else if (cmd == "read") {
        if (parsed.positional.size() != 1) { print_cmd_usage(cmd); return 1; }
        std::cout << read_text_raw(parsed.positional[0].c_str());
    }
    else if (cmd == "write") {
        if (parsed.positional.size() != 2) { print_cmd_usage(cmd); return 1; }
        std::cout << (write_text_raw(parsed.positional[0].c_str(),
                                     parsed.positional[1]) ? "OK" : "FAIL") << std::endl;
    }
    else if (cmd == "chmod") {
        if (parsed.positional.size() != 2) { print_cmd_usage(cmd); return 1; }
        int mode = strtol(parsed.positional[1].c_str(), nullptr, 8);
        std::cout << (chmod(parsed.positional[0].c_str(), mode) == 0 ? "OK" : "FAIL") << std::endl;
    }
    else if (cmd == "stat") {
        if (parsed.positional.size() != 1) { print_cmd_usage(cmd); return 1; }
        std::cout << stat_raw(parsed.positional[0].c_str()) << std::endl;
    }
    else if (cmd == "grep") {
        if (parsed.positional.size() != 2) { print_cmd_usage(cmd); return 1; }
        std::string content = read_text_raw(parsed.positional[0].c_str());
        std::istringstream iss(content);
        std::string line;
        while (std::getline(iss, line)) {
            if (line.find(parsed.positional[1]) != std::string::npos)
                std::cout << line << std::endl;
        }
    }

    else if (cmd == "rename") {
        if (parsed.positional.size() < 3) { print_cmd_usage(cmd); return 1; }
        int n = rename_batch(parsed.positional[0].c_str(),
                             parsed.positional[1],
                             parsed.positional[2], dry_run);
        std::cout << n << I(" files ", " 个文件")
                  << (dry_run ? I(" would be renamed.", "将被重命名。")
                              : I(" renamed.", "已重命名。")) << std::endl;
    }
    else if (cmd == "replace") {
        if (parsed.positional.size() != 3) { print_cmd_usage(cmd); return 1; }
        std::cout << (replace_in_file(parsed.positional[0].c_str(),
                                      parsed.positional[1],
                                      parsed.positional[2]) ? "OK" : "FAIL") << std::endl;
    }
    else if (cmd == "replacedir") {
        if (parsed.positional.size() < 3) { print_cmd_usage(cmd); return 1; }
        std::string ext = parsed.positional.size() >= 4 ? parsed.positional[3] : "";
        int n = replace_in_dir(parsed.positional[0].c_str(),
                               parsed.positional[1],
                               parsed.positional[2], ext, recursive, dry_run);
        std::cout << n << I(" files ", " 个文件")
                  << (dry_run ? I(" would be updated.", "将被更新。")
                              : I(" updated.", "已更新。")) << std::endl;
    }
    else if (cmd == "hexdump") {
        if (parsed.positional.size() < 1) { print_cmd_usage(cmd); return 1; }
        size_t len = 0, off = 0;
        if (parsed.positional.size() >= 2) len = atoi(parsed.positional[1].c_str());
        if (parsed.positional.size() >= 3) off = atoi(parsed.positional[2].c_str());
        hexdump(parsed.positional[0].c_str(), len, off);
    }
    else if (cmd == "hexwrite") {
        if (parsed.positional.size() != 3) { print_cmd_usage(cmd); return 1; }
        long offset = atol(parsed.positional[1].c_str());
        std::cout << (hexwrite(parsed.positional[0].c_str(),
                               offset, parsed.positional[2]) ? "OK" : "FAIL") << std::endl;
    }
    else if (cmd == "diff") {
        if (parsed.positional.size() != 2) { print_cmd_usage(cmd); return 1; }
        diff_files(parsed.positional[0].c_str(), parsed.positional[1].c_str());
    }
#ifdef HAS_LIBZIP
    else if (cmd == "zip") {
        if (parsed.positional.size() < 2) { print_cmd_usage(cmd); return 1; }
        std::string output = parsed.positional[0];
        std::vector<std::string> inputs(parsed.positional.begin() + 1,
                                        parsed.positional.end());
        zip_with_password(output, inputs, password, overwrite);
    }
    else if (cmd == "unzip") {
        if (parsed.positional.size() < 1) { print_cmd_usage(cmd); return 1; }
        std::string zip_path = parsed.positional[0];
        std::string out_dir = parsed.positional.size() >= 2 ? parsed.positional[1] : ".";
        unzip_with_password(zip_path, out_dir, password, overwrite);
    }
#endif
    else if (cmd == "help" || cmd == "--help" || cmd == "-h") {
        print_help();
    }
    else {
        std::cerr << I("Unknown command: ", "未知命令：") << cmd << std::endl;
        print_help();
        return 1;
    }

    return 0;
}
