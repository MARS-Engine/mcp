#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <filesystem>
#include <map>
#include <set>
#include <algorithm>
#include <chrono>
#include <clang-c/Index.h>
#include <nlohmann/json.hpp>

//this is all ai slop but it works, once c++26 releases in clang i will actually clean this up.
// from some quick tests i did  this are how much tokens save with increased error rate, i only did two runs
// and the token count report is from Google AI Studio
// 1 error - 4k -> 700 
// 4 errors - 37k -> 1.3k


namespace fs = std::filesystem;
// Use ordered_json to strictly enforce the exact order of keys in the output payload
using json = nlohmann::ordered_json;

// Global Configuration & State
struct Config {
    std::string build_dir = "build";
    bool show_errors = true;
    bool show_warnings = true;
} cfg;

std::mutex queue_mutex;
std::mutex output_mutex;
int current_index = 0;

json old_cache;
json new_cache;
std::string global_resource_dir = "";

// Helper structure for grouping identical issues
struct IssueKey {
    std::string verbosity;
    std::string message;
    std::string snippet;
    
    // We explicitly DO NOT include context here anymore.
    // This prevents template instantiation spam from duplicating the same error.
    bool operator<(const IssueKey& o) const {
        if (verbosity != o.verbosity) return verbosity < o.verbosity;
        if (message != o.message) return message < o.message;
        return snippet < o.snippet;
    }
};

struct IssueData {
    std::set<std::pair<int, int>> locations;
    std::vector<std::string> contexts; // Store unique JSON dumps of contexts
};

// Helper to get file modification time
long long get_timestamp(const std::string& path) {
    try {
        if (!fs::exists(path)) return 0;
        return std::chrono::duration_cast<std::chrono::milliseconds>(fs::last_write_time(path).time_since_epoch()).count();
    } catch(...) { return 0; }
}

// Extracts the exact line from the file and adds a caret (^) at the specific column
std::vector<std::string> get_source_snippet(const std::string& filepath, unsigned line, unsigned col) {
    if (filepath.empty() || line == 0 || col == 0) return {};
    
    std::ifstream file(filepath);
    if (!file.is_open()) return {};
    
    std::string current_line;
    unsigned current_line_num = 1;
    while (std::getline(file, current_line)) {
        if (current_line_num == line) {
            std::string caret_line = "";
            for (unsigned i = 0; i < col - 1 && i < current_line.length(); ++i) {
                // Match tabs or spaces to keep the caret aligned
                if (current_line[i] == '\t') caret_line += '\t';
                else caret_line += ' ';
            }
            caret_line += "^";
            // Return as an array of strings so JSON doesn't smash it into one line
            return {current_line, caret_line};
        }
        current_line_num++;
    }
    return {};
}

// Tokenizer for cases where 'arguments' field is missing
std::vector<std::string> tokenize(const std::string& cmd) {
    std::vector<std::string> args;
    std::string current;
    bool in_quotes = false;
    for (size_t i = 0; i < cmd.size(); ++i) {
        if (cmd[i] == '\"') in_quotes = !in_quotes;
        else if (cmd[i] == ' ' && !in_quotes) {
            if (!current.empty()) { args.push_back(current); current.clear(); }
        } else current += cmd[i];
    }
    if (!current.empty()) args.push_back(current);
    return args;
}

std::string find_resource_dir(const std::string& compiler_path) {
    try {
        fs::path bin_path = fs::path(compiler_path).parent_path();
        fs::path lib_clang = bin_path.parent_path() / "lib" / "clang";
        if (fs::exists(lib_clang)) {
            for (const auto& entry : fs::directory_iterator(lib_clang)) {
                if (entry.is_directory() && fs::exists(entry.path() / "include")) 
                    return entry.path().string();
            }
        }
    } catch(...) {}
    return "";
}

// Callback for libclang to visit all included files
void inclusion_visitor(CXFile included_file, CXSourceLocation* inclusion_stack, unsigned include_len, CXClientData client_data) {
    auto* deps = static_cast<std::map<std::string, long long>*>(client_data);
    CXString filename = clang_getFileName(included_file);
    std::string path = clang_getCString(filename);
    clang_disposeString(filename);
    
    if (!path.empty()) {
        (*deps)[path] = get_timestamp(path);
    }
}

void parse_worker(const json& compdb, const std::string& project_root, const std::string& build_norm) {
    CXIndex index = clang_createIndex(0, 0); 
    if (!index) return;
    
    std::string root_norm = fs::path(project_root).lexically_normal().generic_string();
    if (root_norm.empty() || root_norm.back() != '/') root_norm += "/";

    while (true) {
        int i;
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            if (current_index >= (int)compdb.size()) break;
            i = current_index++;
        }
        
        const auto& entry = compdb[i];
        std::string file_path = entry["file"];
        std::string dir = entry["directory"];
        std::string file_norm = fs::path(file_path).lexically_normal().generic_string();

        // Skip anything inside the build folder, third_party, or CMake internal PCHs
        if (file_norm.find(build_norm) != std::string::npos) continue;
        if (file_norm.find("third_party") != std::string::npos) continue;
        if (file_path.find("cmake_pch") != std::string::npos) continue;

        long long current_time = get_timestamp(file_path);
        
        // --- Dependency-Aware Cache Check ---
        bool skip_parsing = false;
        {
            std::lock_guard<std::mutex> lock(output_mutex);
            if (current_time != 0 && old_cache.contains("timestamps") && old_cache["timestamps"].value(file_path, 0LL) == current_time) {
                bool was_clean = !old_cache.contains("diagnostics") || 
                                 !old_cache["diagnostics"].contains(file_path) || 
                                 old_cache["diagnostics"][file_path].empty();

                bool deps_changed = false;
                if (old_cache.contains("dependencies") && old_cache["dependencies"].contains(file_path)) {
                    for (auto& [dep_path, old_ts] : old_cache["dependencies"][file_path].items()) {
                        long long cached_dep_ts = old_ts.get<long long>();
                        long long current_dep_ts = get_timestamp(dep_path);
                        if (cached_dep_ts == 0 || current_dep_ts == 0 || current_dep_ts > cached_dep_ts) {
                            deps_changed = true;
                            break;
                        }
                    }
                }

                if (was_clean && !deps_changed) {
                    new_cache["timestamps"][file_path] = current_time;
                    new_cache["diagnostics"][file_path] = json::array();
                    if (old_cache.contains("dependencies"))
                        new_cache["dependencies"][file_path] = old_cache["dependencies"][file_path];
                    skip_parsing = true;
                }
            }
        }
        
        if (skip_parsing) continue;

        {
            std::lock_guard<std::mutex> lock(output_mutex);
            std::cerr << "[*] Parsing: " << fs::path(file_path).filename().string() << std::endl;
        }

        std::vector<std::string> raw_args;
        if (entry.contains("arguments") && entry["arguments"].is_array()) {
            for (const auto& a : entry["arguments"]) raw_args.push_back(a.get<std::string>());
        } else {
            raw_args = tokenize(entry.value("command", ""));
        }

        std::vector<std::string> final_args;
        final_args.push_back("-working-directory=" + dir);
        if (!global_resource_dir.empty()) final_args.push_back("-resource-dir=" + global_resource_dir);
        
        if (!raw_args.empty()) {
            std::string compiler = fs::path(raw_args[0]).filename().string();
            if (compiler.find("cl") != std::string::npos) final_args.push_back("--driver-mode=cl");
        }

        for (size_t j = 1; j < raw_args.size(); ++j) {
            std::string arg = raw_args[j];
            std::string arg_norm = fs::path(arg).lexically_normal().generic_string();
            if (arg_norm == file_norm) continue;
            if (arg == "-o" || arg == "/o" || arg == "-Fo" || arg == "/Fo") {
                if (j + 1 < raw_args.size()) j++; 
                continue;
            }
            final_args.push_back(arg);
        }
        final_args.push_back("-I" + root_norm);

        std::vector<const char*> c_args;
        for (const auto& a : final_args) c_args.push_back(a.c_str());

        CXTranslationUnit tu;
        CXErrorCode err = clang_parseTranslationUnit2(
            index, file_path.c_str(), c_args.data(), (int)c_args.size(), 
            nullptr, 0, CXTranslationUnit_KeepGoing | CXTranslationUnit_DetailedPreprocessingRecord, &tu
        );
        
        json file_diags = json::array();
        std::map<std::string, long long> dependencies;

        if (err == CXError_Success) {
            clang_getInclusions(tu, inclusion_visitor, &dependencies);

            unsigned num_diags = clang_getNumDiagnostics(tu);
            for (unsigned d = 0; d < num_diags; ++d) {
                CXDiagnostic diag = clang_getDiagnostic(tu, d);
                auto severity = clang_getDiagnosticSeverity(diag);
                
                bool is_error = (severity >= CXDiagnostic_Error);
                bool is_warning = (severity == CXDiagnostic_Warning);

                if ((is_error && cfg.show_errors) || (is_warning && cfg.show_warnings)) {
                    CXString cx_msg = clang_getDiagnosticSpelling(diag);
                    std::string msg = clang_getCString(cx_msg);
                    clang_disposeString(cx_msg);
                    
                    CXSourceLocation loc = clang_getDiagnosticLocation(diag);
                    CXFile file;
                    unsigned line, col;
                    clang_getSpellingLocation(loc, &file, &line, &col, nullptr);
                    
                    std::string diag_file = file_path;
                    if (file) {
                        CXString cx_f = clang_getFileName(file);
                        diag_file = clang_getCString(cx_f);
                        clang_disposeString(cx_f);
                    }

                    // Get the actual line of code where the error occurred as an array of strings
                    std::vector<std::string> snippet = get_source_snippet(diag_file, line, col);

                    // Extract Context (Notes and Template Instantiations) as a JSON Array
                    json context_array = json::array();
                    CXDiagnosticSet child_set = clang_getChildDiagnostics(diag);
                    unsigned num_children = clang_getNumDiagnosticsInSet(child_set);

                    for (unsigned c = 0; c < num_children; ++c) {
                        CXDiagnostic child = clang_getDiagnosticInSet(child_set, c);
                        
                        CXString cx_cmsg = clang_getDiagnosticSpelling(child);
                        std::string cmsg = clang_getCString(cx_cmsg);
                        clang_disposeString(cx_cmsg);
                        
                        CXSourceLocation cloc = clang_getDiagnosticLocation(child);
                        CXFile cfile;
                        unsigned cline, ccol;
                        clang_getSpellingLocation(cloc, &cfile, &cline, &ccol, nullptr);
                        
                        std::string cfile_name = "unknown";
                        std::string cfile_path = "";
                        if (cfile) {
                            CXString cx_cf = clang_getFileName(cfile);
                            cfile_path = clang_getCString(cx_cf);
                            cfile_name = fs::path(cfile_path).filename().string();
                            clang_disposeString(cx_cf);
                        }
                        
                        // Clean up verbose inclusion notes to save LLM tokens
                        std::string prefix = "in file included from ";
                        if (cmsg.find(prefix) == 0) {
                            cmsg = "included here";
                        }
                        
                        // Enforce specific key ordering for context notes
                        json child_obj;
                        child_obj["message"] = cmsg;
                        
                        std::vector<std::string> csnippet = get_source_snippet(cfile_path, cline, ccol);
                        if (!csnippet.empty()) {
                            child_obj["snippet"] = csnippet;
                        }
                        
                        child_obj["file"] = cfile_name + ":" + std::to_string(cline) + ":" + std::to_string(ccol);

                        context_array.push_back(child_obj);
                        clang_disposeDiagnostic(child);
                    }

                    CXString cx_opt = clang_getDiagnosticOption(diag, nullptr);
                    std::string opt = clang_getCString(cx_opt) ? clang_getCString(cx_opt) : "";
                    clang_disposeString(cx_opt);

                    json diag_obj = {
                        {"severity", is_error ? "error" : "warning"}, 
                        {"message", msg}, 
                        {"file", diag_file}, 
                        {"line", line},
                        {"col", col}
                    };
                    if (!snippet.empty()) diag_obj["snippet"] = snippet; // Assigned as JSON array natively
                    if (!opt.empty()) diag_obj["flag"] = opt;
                    if (!context_array.empty()) diag_obj["context"] = context_array;

                    file_diags.push_back(diag_obj);
                }
                clang_disposeDiagnostic(diag);
            }
            clang_disposeTranslationUnit(tu);
        }
        
        {
            std::lock_guard<std::mutex> lock(output_mutex);
            new_cache["timestamps"][file_path] = current_time;
            new_cache["diagnostics"][file_path] = file_diags;
            new_cache["dependencies"][file_path] = dependencies;
        }
    }
    clang_disposeIndex(index);
}

void print_llm_payload(const std::string& project_root) {
    json llm_payload = json::object();
    std::string root_norm = fs::path(project_root).lexically_normal().generic_string();
    if (root_norm.empty() || root_norm.back() != '/') root_norm += "/";

    for (auto& [file_path, file_diags] : new_cache["diagnostics"].items()) {
        if (file_diags.empty()) continue;
        
        // Group by path and pure IssueKey (no context)
        std::map<std::string, std::map<IssueKey, IssueData>> grouped;
        
        for (auto& diag : file_diags) {
            std::string d_file = fs::path((std::string)diag["file"]).lexically_normal().generic_string();
            std::string display_path;

            if (d_file.find(root_norm) == 0) display_path = d_file.substr(root_norm.length());
            else display_path = "External/" + fs::path(d_file).filename().string();

            IssueKey key;
            key.verbosity = diag["severity"];
            key.message = diag["message"];
            if (diag.contains("flag")) key.message += " [" + (std::string)diag["flag"] + "]";
            
            // Dump the array to string so it can be used in the map grouping key
            if (diag.contains("snippet")) key.snippet = diag["snippet"].dump();
            
            auto& data = grouped[display_path][key];
            data.locations.insert({(int)diag["line"], (int)diag["col"]});
            
            // Only store the VERY FIRST context trace for this error. 
            if (diag.contains("context")) {
                if (data.contexts.empty()) {
                    data.contexts.push_back(diag["context"].dump());
                }
            }
        }
        
        for (auto& [path, issues] : grouped) {
            for (auto& [key, data] : issues) {
                // Initialize ordered_json to guarantee layout shape
                json issue_obj;
                issue_obj["verbosity"] = key.verbosity;
                issue_obj["message"] = key.message;
                issue_obj["instances"] = data.locations.size(); // NEW: Include instance count

                // Parse the dumped snippet string back into a JSON array
                if (!key.snippet.empty()) {
                    issue_obj["snippet"] = json::parse(key.snippet);
                }

                // Format as file:line:col
                if (data.locations.size() == 1) {
                    issue_obj["file"] = path + ":" + std::to_string(data.locations.begin()->first) + ":" + std::to_string(data.locations.begin()->second);
                } else {
                    std::string l_str;
                    int count = 0;
                    for (const auto& loc : data.locations) {
                        if (count++ > 0) l_str += ", ";
                        l_str += std::to_string(loc.first) + ":" + std::to_string(loc.second);
                        if (count >= 4 && data.locations.size() > 4) {
                            l_str += " ... (+" + std::to_string(data.locations.size() - 3) + " more)";
                            break;
                        }
                    }
                    issue_obj["file"] = path + " at " + l_str;
                }

                // Add the single grouped context trace
                if (!data.contexts.empty()) {
                    issue_obj["context"] = json::parse(data.contexts[0]);
                }

                llm_payload[path].push_back(issue_obj);
            }
        }
    }

    if (llm_payload.empty()) {
        llm_payload["status"] = "successful build";
    }

    std::cout << llm_payload.dump(2) << std::endl;
}

int main(int argc, char** argv) {
    bool explicit_filter = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--errors") { cfg.show_errors = true; explicit_filter = true; }
        else if (arg == "--warnings") { cfg.show_warnings = true; explicit_filter = true; }
        else if (arg[0] != '-') { cfg.build_dir = arg; }
    }
    
    if (explicit_filter) {
        bool err_passed = false, warn_passed = false;
        for (int i = 1; i < argc; ++i) {
            if (std::string(argv[i]) == "--errors") err_passed = true;
            if (std::string(argv[i]) == "--warnings") warn_passed = true;
        }
        cfg.show_errors = err_passed;
        cfg.show_warnings = warn_passed;
    }

    std::string db_path = cfg.build_dir + "/compile_commands.json";
    if (!fs::exists(db_path)) {
        std::cerr << "[!] Could not find " << db_path << std::endl;
        return 1;
    }

    std::ifstream f(db_path);
    json compdb = json::parse(f);

    if (!compdb.empty()) {
        std::vector<std::string> args;
        if (compdb[0].contains("arguments")) {
            for (auto& a : compdb[0]["arguments"]) args.push_back(a.get<std::string>());
        } else {
            args = tokenize(compdb[0].value("command", ""));
        }
        if (!args.empty()) global_resource_dir = find_resource_dir(args[0]);
    }

    std::string cache_file = cfg.build_dir + "/mcp_cache.json";
    std::ifstream c(cache_file);
    if (c.is_open()) try { c >> old_cache; } catch(...) {}

    unsigned int threads = std::thread::hardware_concurrency();
    std::vector<std::thread> workers;
    std::string root = fs::current_path().string();
    std::string build_norm = fs::path(cfg.build_dir).lexically_normal().generic_string();
    
    std::cerr << "[*] Starting parse. Filters: Errors=" << (cfg.show_errors?"ON":"OFF") 
              << " Warnings=" << (cfg.show_warnings?"ON":"OFF") << std::endl;

    for (unsigned int i = 0; i < (threads ? threads : 4); ++i) 
        workers.emplace_back(parse_worker, std::ref(compdb), root, build_norm);
    
    for (auto& w : workers) w.join();

    std::ofstream oc(cache_file);
    oc << new_cache.dump(2);
    
    print_llm_payload(root);
    return 0;
}