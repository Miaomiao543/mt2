

#include <unordered_map>


#define main mt2_main_internal
#include "mt2_v3.cpp"
#undef main


extern "C" {


const char* mt2_version() {
    return "mt2 v3.0 (shared library build)";
}


int mt2_has_zip_support() {
#ifdef HAS_LIBZIP
    return 1;
#else
    return 0;
#endif
}


int mt2_exec(int argc, const char** argv) {
    if (argc < 1 || !argv) return 1;

    install_signal_handlers();

    const char* env_lang = std::getenv("MT2_LANG");
    if (env_lang && std::string(env_lang) == "en") {
        g_lang = Lang::EN;
    }

    std::string cmd = argv[0];


    std::vector<std::string> arg_vec;
    for (int i = 1; i < argc; i++) arg_vec.push_back(argv[i]);

    auto parsed = parse_args(arg_vec);
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


    if (cmd == "mt2") return 1;


    {
        std::string exe_dir = get_executable_dir();
        if (!exe_dir.empty()) {
            std::string external_path = exe_dir + "/mt2_" + cmd;
            if (access(external_path.c_str(), X_OK) == 0) {
                std::vector<const char*> exec_args;
                exec_args.push_back(external_path.c_str());
                for (const auto& a : arg_vec) exec_args.push_back(a.c_str());
                exec_args.push_back(nullptr);

                pid_t pid = fork();
                if (pid == 0) {
                    execvp(external_path.c_str(), (char* const*)exec_args.data());
                    _exit(127);
                } else if (pid > 0) {
                    int status;
                    waitpid(pid, &status, 0);
                    if (WIFEXITED(status)) return WEXITSTATUS(status);
                    else if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
                    return 1;
                }
                return 1;
            }
        }
    }


    if (cmd == "copy") {
        if (parsed.positional.size() != 2) { print_cmd_usage(cmd); return 1; }
        return copy_file_raw(parsed.positional[0].c_str(),
                             parsed.positional[1].c_str()) ? 0 : 1;
    }
    else if (cmd == "move") {
        if (parsed.positional.size() != 2) { print_cmd_usage(cmd); return 1; }
        return move_file_raw(parsed.positional[0].c_str(),
                             parsed.positional[1].c_str()) ? 0 : 1;
    }
    else if (cmd == "delete") {
        if (parsed.positional.size() != 1) { print_cmd_usage(cmd); return 1; }
        return delete_file_raw(parsed.positional[0].c_str(), false, force) == 0 ? 0 : 1;
    }
    else if (cmd == "list") {
        if (parsed.positional.size() != 1) { print_cmd_usage(cmd); return 1; }
        auto files = list_dir_raw(parsed.positional[0].c_str());
        for (const auto& f : files) std::cout << f << std::endl;
        return 0;
    }
    else if (cmd == "read") {
        if (parsed.positional.size() != 1) { print_cmd_usage(cmd); return 1; }
        std::cout << read_text_raw(parsed.positional[0].c_str());
        return 0;
    }
    else if (cmd == "write") {
        if (parsed.positional.size() != 2) { print_cmd_usage(cmd); return 1; }
        return write_text_raw(parsed.positional[0].c_str(),
                              parsed.positional[1]) ? 0 : 1;
    }
    else if (cmd == "chmod") {
        if (parsed.positional.size() != 2) { print_cmd_usage(cmd); return 1; }
        int mode = strtol(parsed.positional[1].c_str(), nullptr, 8);
        return chmod(parsed.positional[0].c_str(), mode) == 0 ? 0 : 1;
    }
    else if (cmd == "stat") {
        if (parsed.positional.size() != 1) { print_cmd_usage(cmd); return 1; }
        std::cout << stat_raw(parsed.positional[0].c_str()) << std::endl;
        return 0;
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
        return 0;
    }
    else if (cmd == "rename") {
        if (parsed.positional.size() < 3) { print_cmd_usage(cmd); return 1; }
        int n = rename_batch(parsed.positional[0].c_str(),
                             parsed.positional[1], parsed.positional[2], dry_run);
        std::cout << n << I(" files ", " 个文件")
                  << (dry_run ? I(" would be renamed.", "将被重命名。")
                              : I(" renamed.", "已重命名。")) << std::endl;
        return 0;
    }
    else if (cmd == "replace") {
        if (parsed.positional.size() != 3) { print_cmd_usage(cmd); return 1; }
        return replace_in_file(parsed.positional[0].c_str(),
                               parsed.positional[1], parsed.positional[2]) ? 0 : 1;
    }
    else if (cmd == "replacedir") {
        if (parsed.positional.size() < 3) { print_cmd_usage(cmd); return 1; }
        std::string ext = parsed.positional.size() >= 4 ? parsed.positional[3] : "";
        int n = replace_in_dir(parsed.positional[0].c_str(),
                               parsed.positional[1], parsed.positional[2],
                               ext, recursive, dry_run);
        std::cout << n << I(" files ", " 个文件")
                  << (dry_run ? I(" would be updated.", "将被更新。")
                              : I(" updated.", "已更新。")) << std::endl;
        return 0;
    }
    else if (cmd == "hexdump") {
        if (parsed.positional.size() < 1) { print_cmd_usage(cmd); return 1; }
        size_t len = 0, off = 0;
        if (parsed.positional.size() >= 2) len = atoi(parsed.positional[1].c_str());
        if (parsed.positional.size() >= 3) off = atoi(parsed.positional[2].c_str());
        hexdump(parsed.positional[0].c_str(), len, off);
        return 0;
    }
    else if (cmd == "hexwrite") {
        if (parsed.positional.size() != 3) { print_cmd_usage(cmd); return 1; }
        long offset = atol(parsed.positional[1].c_str());
        return hexwrite(parsed.positional[0].c_str(), offset,
                        parsed.positional[2]) ? 0 : 1;
    }
    else if (cmd == "diff") {
        if (parsed.positional.size() != 2) { print_cmd_usage(cmd); return 1; }
        diff_files(parsed.positional[0].c_str(), parsed.positional[1].c_str());
        return 0;
    }
#ifdef HAS_LIBZIP
    else if (cmd == "zip") {
        if (parsed.positional.size() < 2) { print_cmd_usage(cmd); return 1; }
        std::string output = parsed.positional[0];
        std::vector<std::string> inputs(parsed.positional.begin() + 1,
                                        parsed.positional.end());
        zip_with_password(output, inputs, password, overwrite);
        return 0;
    }
    else if (cmd == "unzip") {
        if (parsed.positional.size() < 1) { print_cmd_usage(cmd); return 1; }
        std::string zip_path = parsed.positional[0];
        std::string out_dir = parsed.positional.size() >= 2 ? parsed.positional[1] : ".";
        unzip_with_password(zip_path, out_dir, password, overwrite);
        return 0;
    }
#endif
    else if (cmd == "help" || cmd == "--help" || cmd == "-h") {
        print_help();
        return 0;
    }

    std::cerr << I("Unknown command: ", "未知命令：") << cmd << std::endl;
    return 1;
}


int mt2_exec_str(const char* cmd_str) {
    if (!cmd_str) return 1;

    std::vector<std::string> tokens;
    std::string current;
    bool in_quote = false;

    for (const char* p = cmd_str; *p; p++) {
        if (*p == '"') {
            in_quote = !in_quote;
        } else if (*p == ' ' && !in_quote) {
            if (!current.empty()) {
                tokens.push_back(current);
                current.clear();
            }
        } else {
            current += *p;
        }
    }
    if (!current.empty()) tokens.push_back(current);

    if (tokens.empty()) return 1;

    std::vector<const char*> argv;
    for (const auto& t : tokens) argv.push_back(t.c_str());

    return mt2_exec(argv.size(), argv.data());
}


void mt2_set_language(int lang) {
    g_lang = (lang == 1) ? Lang::EN : Lang::ZH;
}


const char* mt2_help() {
    static std::string help_text;
    help_text =
        "mt2.so - Termux File Manager Shared Library\n"
        "============================================\n\n"
        "Exported API (C linkage):\n"
        "  const char* mt2_version()          Get version string\n"
        "  int         mt2_has_zip_support()  Check if ZIP is available (1/0)\n"
        "  int         mt2_exec(argc, argv)   Execute command with args array\n"
        "  int         mt2_exec_str(cmd)      Execute command string\n"
        "  void        mt2_set_language(lang) Set language (0=zh, 1=en)\n"
        "  const char* mt2_help()            Get this help text\n\n"
        "Supported commands:\n"
        "  copy move delete list read write chmod stat grep\n"
        "  rename replace replacedir hexdump hexwrite diff\n"
        "  zip unzip (if compiled with libzip)\n\n"
        "Usage example (C):\n"
        "  void* h = dlopen(\"mt2.so\", RTLD_LAZY);\n"
        "  int (*exec)(int, const char**) = dlsym(h, \"mt2_exec\");\n"
        "  const char* args[] = {\"copy\", \"/sdcard/a\", \"/sdcard/b\"};\n"
        "  int ret = exec(3, args);\n\n"
        "Usage example (Java/Android):\n"
        "  System.loadLibrary(\"mt2\");\n"
        "  // via JNI bridge to mt2_exec_str\n";
    return help_text.c_str();
}


__attribute__((constructor))
static void mt2_so_init() {
    install_signal_handlers();
}

}
