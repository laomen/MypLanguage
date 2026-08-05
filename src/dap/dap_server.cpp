// myp_debug — MYP Debug Adapter Protocol (DAP) server.
//
// Bridges VS Code (DAP over stdin/stdout) to gdb (MI2 interpreter) so MYP
// programs compiled with `-g` can be debugged (breakpoints, stepping, stack,
// locals). M7 core: initialize/launch/setBreakpoints/configurationDone/
// continue/next/stepIn/stepOut/threads/stackTrace/scopes/variables/evaluate/
// pause/disconnect.
//
// Build: ../../mypc? No — plain C++, see CMakeLists.txt (myp_debug target).
// Usage:  myp_debug   (DAP client speaks over stdin/stdout, gdb spawned inside)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

// ============================== minimal JSON ==============================
struct JVal {
    enum T { NUL, BOOL, NUM, STR, ARR, OBJ } t = NUL;
    bool b = false;
    double num = 0;
    std::string s;
    std::vector<JVal> arr;
    std::vector<std::pair<std::string, JVal>> obj;

    const JVal* get(const std::string& k) const {
        for (auto& p : obj) if (p.first == k) return &p.second;
        return nullptr;
    }
    const JVal* getStr(const std::string& k, std::string& out) const {
        auto* v = get(k);
        if (v && v->t == JVal::STR) { out = v->s; return v; }
        return nullptr;
    }
};

static void skipWs(const std::string& t, size_t& i) {
    while (i < t.size() && (t[i] == ' ' || t[i] == '\t' || t[i] == '\n' || t[i] == '\r')) i++;
}
static std::string jsonUnescape(const std::string& t, size_t& i) {
    // t[i] == '"'
    std::string out;
    i++;
    while (i < t.size()) {
        char c = t[i];
        if (c == '\\') {
            char e = t[++i];
            switch (e) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'u': { i += 4; break; }  // ignore \uXXXX (rare in DAP)
            default: out += e; break;
            }
        } else if (c == '"') { i++; break; }
        else out += c;
        i++;
    }
    return out;
}
static JVal parseJson(const std::string& t, size_t& i) {
    skipWs(t, i);
    JVal v;
    if (i >= t.size()) return v;
    char c = t[i];
    if (c == '{') {
        v.t = JVal::OBJ;
        i++;
        skipWs(t, i);
        if (i < t.size() && t[i] == '}') { i++; return v; }
        while (i < t.size()) {
            skipWs(t, i);
            std::string key = jsonUnescape(t, i);
            skipWs(t, i);
            if (i < t.size() && t[i] == ':') i++;
            skipWs(t, i);
            v.obj.emplace_back(key, parseJson(t, i));
            skipWs(t, i);
            if (i < t.size() && t[i] == ',') { i++; continue; }
            if (i < t.size() && t[i] == '}') { i++; break; }
        }
    } else if (c == '[') {
        v.t = JVal::ARR;
        i++;
        skipWs(t, i);
        if (i < t.size() && t[i] == ']') { i++; return v; }
        while (i < t.size()) {
            v.arr.push_back(parseJson(t, i));
            skipWs(t, i);
            if (i < t.size() && t[i] == ',') { i++; continue; }
            if (i < t.size() && t[i] == ']') { i++; break; }
        }
    } else if (c == '"') {
        v.t = JVal::STR;
        v.s = jsonUnescape(t, i);
    } else if (c == 't' && t.compare(i, 4, "true") == 0) {
        v.t = JVal::BOOL; v.b = true; i += 4;
    } else if (c == 'f' && t.compare(i, 5, "false") == 0) {
        v.t = JVal::BOOL; v.b = false; i += 5;
    } else if (c == 'n' && t.compare(i, 4, "null") == 0) {
        v.t = JVal::NUL; i += 4;
    } else if (c == '-' || (c >= '0' && c <= '9')) {
        v.t = JVal::NUM;
        size_t j = i;
        if (t[j] == '-') j++;
        while (j < t.size() && (isdigit((unsigned char)t[j]) || t[j] == '.')) j++;
        v.num = atof(t.substr(i, j - i).c_str());
        i = j;
    } else {
        i++;  // skip unknown
    }
    return v;
}

static std::string jsonEscape(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\t': o += "\\t"; break;
        case '\r': o += "\\r"; break;
        default: o += c; break;
        }
    }
    return o;
}

// ============================== DAP server ==============================
class DapServer {
public:
    int run() {
        while (true) {
            pollfd fds[2];
            fds[0] = {STDIN_FILENO, POLLIN, 0};
            int nfds = 1;
            if (gdb_read_ >= 0) {
                fds[1] = {gdb_read_, POLLIN, 0};   // read gdb stdout (async events)
                nfds = 2;
            }
            int pr = poll(fds, nfds, -1);
            if (pr < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (fds[0].revents & POLLIN) {
                if (!readDapMessage()) break;  // EOF → exit
            }
            if (nfds == 2 && (fds[1].revents & POLLIN)) {
                pumpGdb();
            }
        }
        shutdownGdb();
        return 0;
    }

private:
    int gdb_fd_ = -1;          // gdb stdin (write side)
    pid_t gdb_pid_ = 0;
    int seq_ = 0;
    bool running_ = false;
    bool started_ = false;   // -exec-run issued yet
    std::string exe_;
    int next_bp_id_ = 1;
    std::map<int, std::pair<std::string, int>> breakpoints_;  // dapId -> (file,line)

    // ---- DAP framing (Content-Length header + JSON body) ----
    std::string dapBuf_;
    bool readDapMessage() {
        char buf[4096];
        ssize_t n = ::read(STDIN_FILENO, buf, sizeof(buf));
        if (n <= 0) return false;
        dapBuf_.append(buf, n);
        // Find Content-Length header.
        while (true) {
            size_t hdrEnd = dapBuf_.find("\r\n\r\n");
            if (hdrEnd == std::string::npos) return true;
            std::string hdr = dapBuf_.substr(0, hdrEnd);
            size_t cl = hdr.find("Content-Length:");
            if (cl == std::string::npos) return true;
            long len = atol(hdr.c_str() + cl + 15);
            if (dapBuf_.size() < hdrEnd + 4 + (size_t)len) return true;
            std::string body = dapBuf_.substr(hdrEnd + 4, len);
            dapBuf_.erase(0, hdrEnd + 4 + len);
            handleRequest(body);
        }
        return true;
    }

    void sendRaw(const std::string& body) {
        std::string frame = "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
        ::write(STDOUT_FILENO, frame.data(), frame.size());
        fflush(stdout);
    }
    void sendResponse(int id, const std::string& body) {
        sendRaw("{\"type\":\"response\",\"seq\":" + std::to_string(++seq_) +
                ",\"request_seq\":" + std::to_string(id) +
                ",\"success\":true,\"command\":\"\",\"body\":" + body + "}");
    }
    void sendEvent(const std::string& method, const std::string& params) {
        sendRaw("{\"type\":\"event\",\"seq\":" + std::to_string(++seq_) +
                ",\"event\":\"" + method + "\",\"body\":" + params + "}");
    }
    void sendStopped(const std::string& reason) {
        sendEvent("stopped", "{\"reason\":\"" + reason + "\",\"threadId\":1,\"allThreadsStopped\":true}");
    }

    // ---- gdb subprocess ----
    bool spawnGdb() {
        int in[2], out[2];
        if (pipe(in) || pipe(out)) return false;
        pid_t pid = fork();
        if (pid < 0) return false;
        if (pid == 0) {
            // child: gdb stdin = in[0], gdb stdout/stderr = out[1]
            dup2(in[0], STDIN_FILENO);
            dup2(out[1], STDOUT_FILENO);
            dup2(out[1], STDERR_FILENO);
            close(in[0]); close(in[1]); close(out[0]); close(out[1]);
            execlp("gdb", "gdb", "-q", "--interpreter=mi2", nullptr);
            _exit(127);
        }
        gdb_pid_ = pid;
        gdb_fd_ = in[1];          // parent writes to gdb stdin
        gdb_read_ = out[0];       // parent reads gdb stdout
        return true;
    }
    int gdb_read_ = -1;

    void gdbSend(const std::string& cmd) {
        std::string line = cmd + "\n";
        ::write(gdb_fd_, line.data(), line.size());
    }

    // Read one MI response line matching `token` (e.g. "^done,.."). Returns
    // the payload after the first comma. Async/notify lines (`*`/`=`) are
    // buffered into gdbBuf_ for the main loop (pumpGdb) to process — they are
    // NOT handled here to avoid nested gdbWait reentrancy.
    std::string gdbWait(const std::string& token) {
        std::string acc;
        char buf[8192];
        while (true) {
            ssize_t n = ::read(gdb_read_, buf, sizeof(buf));
            if (n <= 0) break;
            acc.append(buf, n);
            while (true) {
                size_t nl = acc.find('\n');
                if (nl == std::string::npos) break;
                std::string line = acc.substr(0, nl);
                acc.erase(0, nl + 1);
                if (!line.empty() && line[0] == '^') {
                    if (token.empty() || line.compare(0, token.size(), token) == 0) {
                        size_t c = line.find(',');
                        return c == std::string::npos ? "" : line.substr(c + 1);
                    }
                } else if (!line.empty() && (line[0] == '*' || line[0] == '=')) {
                    gdbBuf_ += line;
                    gdbBuf_ += '\n';
                }
            }
        }
        return "";
    }

    void pumpGdb() {
        // Single non-blocking-ish read: drain whatever gdb produced; the main
        // loop re-polls, so a blocking read here would stall stdin handling.
        char buf[8192];
        ssize_t n = ::read(gdb_read_, buf, sizeof(buf));
        if (n > 0) {
            gdbBuf_.append(buf, n);
            drainGdbLines();
        }
        // n <= 0: no data right now (gdb idle) or gdb closed — return to poll.
    }
    std::string gdbBuf_;
    void drainGdbLines() {
        while (true) {
            size_t nl = gdbBuf_.find('\n');
            if (nl == std::string::npos) break;
            std::string line = gdbBuf_.substr(0, nl);
            gdbBuf_.erase(0, nl + 1);
            if (!line.empty() && line[0] == '*') handleGdbAsync(line);
            else if (!line.empty() && line[0] == '=') handleGdbNotify(line);
        }
    }

    // ---- DAP request handlers ----
    void handleRequest(const std::string& body) {
        size_t p = 0;
        JVal msg = parseJson(body, p);
        int id = 0;
        if (auto* v = msg.get("id")) id = (int)v->num;
        std::string cmd;
        msg.getStr("command", cmd);
        const JVal* args = msg.get("arguments");

        if (cmd == "initialize") {
            sendResponse(id, "{\"supportsConfigurationDoneRequest\":true,"
                             "\"supportsEvaluateForHovers\":true,\"supportsStepBack\":false,"
                             "\"supportsTerminateRequest\":true}");
            sendEvent("initialized", "{}");
        } else if (cmd == "launch") {
            args->getStr("program", exe_);
            if (exe_.empty()) args->getStr("target", exe_);
            if (spawnGdb()) {
                started_ = false;
                gdbSend("-file-exec-and-symbols " + exe_);
                gdbWait("^done");
                sendResponse(id, "{}");
            } else {
                sendRaw("{\"type\":\"response\",\"seq\":" + std::to_string(++seq_) +
                        ",\"request_seq\":" + std::to_string(id) +
                        ",\"success\":false,\"command\":\"launch\",\"message\":\"cannot spawn gdb\"}");
            }
        } else if (cmd == "setBreakpoints") {
            std::string path;
            if (auto* src = args->get("source")) src->getStr("path", path);
            // Clear previous breakpoints for this file.
            std::vector<int> toDel;
            for (auto& kv : breakpoints_)
                if (kv.second.first == path) toDel.push_back(kv.first);
            for (int id2 : toDel) { gdbSend("-break-delete " + std::to_string(id2)); gdbWait("^done"); breakpoints_.erase(id2); }
            std::string res = "[";
            bool first = true;
            if (auto* bp = args->get("breakpoints")) {
                for (auto& b : bp->arr) {
                    auto* lv = b.get("line");
                    int line = lv ? (int)lv->num : 0;
                    gdbSend("-break-insert -f " + path + ":" + std::to_string(line));
                    std::string payload = gdbWait("^done");
                    // Extract bkpt number.
                    std::string num = "0";
                    size_t nk = payload.find("number=\"");
                    if (nk != std::string::npos)
                        num = payload.substr(nk + 8, payload.find('"', nk + 8) - nk - 8);
                    int dapId = next_bp_id_++;
                    breakpoints_[dapId] = {path, line};
                    if (!first) res += ",";
                    first = false;
                    res += "{\"id\":" + std::to_string(dapId) +
                           ",\"verified\":true,\"source\":{\"path\":\"" + jsonEscape(path) + "\"},\"line\":" + std::to_string(line) + "}";
                }
            }
            res += "]";
            sendResponse(id, "{\"breakpoints\":" + res + "}");
        } else if (cmd == "configurationDone") {
            sendResponse(id, "{}");
        } else if (cmd == "continue") {
            running_ = true;
            if (!started_) {
                gdbSend("-exec-run");
                started_ = true;
            } else {
                gdbSend("-exec-continue");
            }
            gdbWait("^running");
            sendResponse(id, "{\"allThreadsContinued\":true}");
        } else if (cmd == "next") {
            gdbSend("-exec-next"); gdbWait("^running");
            sendResponse(id, "{}");
        } else if (cmd == "stepIn") {
            gdbSend("-exec-step"); gdbWait("^running");
            sendResponse(id, "{}");
        } else if (cmd == "stepOut") {
            gdbSend("-exec-finish"); gdbWait("^running");
            sendResponse(id, "{}");
        } else if (cmd == "pause") {
            gdbSend("-exec-interrupt"); gdbWait("^done");
            sendResponse(id, "{}");
        } else if (cmd == "threads") {
            sendResponse(id, "{\"threads\":[{\"id\":1,\"name\":\"main\"}]}");
        } else if (cmd == "stackTrace") {
            gdbSend("-stack-list-frames");
            std::string payload = gdbWait("^done");
            std::string frames = "["; bool first = true;
            size_t pos = payload.find("stack=[");
            if (pos != std::string::npos) {
                // stack=[frame={level="0",addr="0x...",func="main",file="x.myp",fullname="...",line="N",..},...]
                size_t i = pos + 7;
                while (i < payload.size()) {
                    size_t fl = payload.find("frame={", i);
                    if (fl == std::string::npos) break;
                    size_t fe = payload.find('}', fl);
                    if (fe == std::string::npos) break;
                    std::string f = payload.substr(fl + 7, fe - fl - 7);
                    auto field = [&](const char* k) {
                        std::string key = std::string(k) + "=\"";
                        size_t kp = f.find(key);
                        if (kp == std::string::npos) return std::string("");
                        size_t vs = kp + key.size();
                        return f.substr(vs, f.find('"', vs) - vs);
                    };
                    if (!first) frames += ",";
                    first = false;
                    frames += "{\"id\":" + field("level") +
                              ",\"name\":\"" + jsonEscape(field("func")) +
                              "\",\"source\":{\"path\":\"" + jsonEscape(field("file")) +
                              "\"},\"line\":" + field("line") + "}";
                    i = fe + 1;
                }
            }
            frames += "]";
            sendResponse(id, "{\"stackFrames\":" + frames + ",\"totalFrames\":0}");
        } else if (cmd == "scopes") {
            sendResponse(id, "{\"scopes\":[{\"name\":\"Locals\",\"variablesReference\":" +
                std::to_string(frameVarsRef_) + ",\"expensive\":false}]}");
        } else if (cmd == "variables") {
            int ref = 0;
            if (auto* v = args->get("variablesReference")) ref = (int)v->num;
            std::string res = "[";
            bool first = true;
            if (ref == frameVarsRef_) {
                for (auto& v : frameVars_) {
                    if (!first) res += ",";
                    first = false;
                    res += "{\"name\":\"" + jsonEscape(v.first) + "\",\"value\":\"" +
                           jsonEscape(v.second) + "\",\"variablesReference\":0,\"type\":\"auto\"}";
                }
            }
            res += "]";
            sendResponse(id, "{\"variables\":" + res + "}");
        } else if (cmd == "evaluate") {
            std::string expr;
            args->getStr("expression", expr);
            gdbSend("-data-evaluate-expression \"" + expr + "\"");
            std::string payload = gdbWait("^done");
            std::string val;
            size_t vp = payload.find("value=\"");
            if (vp != std::string::npos) {
                size_t vs = vp + 7;
                val = payload.substr(vs, payload.find('"', vs) - vs);
            }
            sendResponse(id, "{\"result\":\"" + jsonEscape(val) + "\",\"variablesReference\":0}");
        } else if (cmd == "disconnect" || cmd == "terminate") {
            sendResponse(id, "{}");
            shutdownGdb();
            sendEvent("terminated", "{}");
        } else {
            // Unknown request — respond success with empty body.
            sendResponse(id, "{}");
        }
    }

    int frameVarsRef_ = 1;
    std::vector<std::pair<std::string, std::string>> frameVars_;

    void handleGdbAsync(const std::string& line) {
        if (line.compare(0, 9, "*stopped,") == 0) {
            running_ = false;
            // reason: breakpoint-hit / end-stepping-range / signal-received
            std::string reason = "unknown";
            if (line.find("reason=\"breakpoint-hit\"") != std::string::npos) reason = "breakpoint";
            else if (line.find("reason=\"end-stepping-range\"") != std::string::npos) reason = "step";
            else if (line.find("reason=\"signal-received\"") != std::string::npos) reason = "exception";
            else if (line.find("reason=\"exited-normally\"") != std::string::npos) reason = "exited";
            else if (line.find("reason=\"exited\"") != std::string::npos) reason = "exited";
            // Refresh current frame locals for the variables view.
            gdbSend("-stack-list-variables 2");
            std::string payload = gdbWait("^done");
            frameVars_.clear();
            size_t pos = payload.find("variables=[");
            if (pos != std::string::npos) {
                size_t i = pos + 11;
                while (true) {
                    size_t vp = payload.find("name=\"", i);
                    if (vp == std::string::npos) break;
                    size_t vs = vp + 6;
                    std::string name = payload.substr(vs, payload.find('"', vs) - vs);
                    size_t valp = payload.find("value=\"", vp);
                    std::string val;
                    if (valp != std::string::npos && valp < payload.size()) {
                        size_t vvs = valp + 7;
                        val = payload.substr(vvs, payload.find('"', vvs) - vvs);
                    }
                    frameVars_.emplace_back(name, val);
                    i = vp + 6;
                    // advance past this name occurrence
                    if (payload.find("name=\"", i) == std::string::npos) break;
                }
            }
            if (reason == "exited") sendEvent("exited", "{\"exitCode\":0}");
            else sendStopped(reason);
        } else if (line.compare(0, 8, "*running") == 0) {
            running_ = true;
        }
    }
    void handleGdbNotify(const std::string&) {}

    void shutdownGdb() {
        if (gdb_pid_ > 0) {
            gdbSend("-gdb-exit");
            for (int i = 0; i < 50; i++) {
                int st = 0;
                if (waitpid(gdb_pid_, &st, WNOHANG) == gdb_pid_) break;
                usleep(20000);
            }
            if (gdb_fd_ >= 0) close(gdb_fd_);
            if (gdb_read_ >= 0) close(gdb_read_);
            gdb_fd_ = -1; gdb_read_ = -1; gdb_pid_ = 0;
        }
    }
};

int main() {
    signal(SIGPIPE, SIG_IGN);
    DapServer srv;
    return srv.run();
}
