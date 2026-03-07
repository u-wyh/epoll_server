#include <iostream>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <queue>
#include <functional>
#include <condition_variable>
#include <sstream>
#include <fstream>
#include <csignal>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <unordered_map>
#include <sys/stat.h>
#include <nlohmann/json.hpp>
#include <sys/wait.h>
#include <sys/types.h>

using json = nlohmann::json;
using namespace std;

struct Config {
    int thread_num = 4;
    int server_port = 8080;
    string log_file = "server.log";
    int monitor_interval = 1000;
    int log_batch = 256;
    int log_flush = 50;

    double cpu_warn = 80.0;
    double cpu_error = 120.0;
    double mem_warn = 100.0;
    double mem_error = 200.0;
    int tps_warn = 10000;
    int tps_error = 20000;
};

enum class LogLevel {
    DEBUG = 0, INFO = 1, WARN = 2, ERROR = 3
};

class AsyncLogger {
private:
    vector<string> buffer1, buffer2;
    atomic<bool> swap_flag{false};
    mutex mtx;
    condition_variable cv;
    atomic<bool> running{true};
    thread worker;
    ofstream fout;
    atomic<LogLevel> level;
    size_t batch;
    int flush_ms;
    bool shutdown_called{false};

public:
    string now_time() {
        auto now = chrono::system_clock::now();
        time_t t = chrono::system_clock::to_time_t(now);
        tm tm_buf{};
        localtime_r(&t, &tm_buf);
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_buf);
        return string(buf);
    }

    void process() {
        vector<string> *current, *back;
        while (running || !buffer1.empty() || !buffer2.empty()) {
            {
                unique_lock<mutex> lock(mtx);
                cv.wait_for(lock, chrono::milliseconds(flush_ms), [this]() {
                    return !buffer1.empty() || !buffer2.empty() || !running;
                });
                swap_flag = !swap_flag;
                if (swap_flag) { current = &buffer1; back = &buffer2; }
                else           { current = &buffer2; back = &buffer1; }
                current->swap(*back);
            }
            for (auto &line : *current) {
                cout << line << "\n";
                if (fout.is_open()) fout << line << "\n";
            }
            if (fout.is_open()) fout.flush();
            current->clear();
        }
    }

    AsyncLogger(const string &filename, LogLevel lvl = LogLevel::INFO,
                size_t batch_size = 256, int flush_interval = 50)
        : batch(batch_size), flush_ms(flush_interval), level(lvl)
    {
        fout.open(filename, ios::app);
        buffer1.reserve(batch * 2);
        buffer2.reserve(batch * 2);
        worker = thread(&AsyncLogger::process, this);
    }

    AsyncLogger(const AsyncLogger&) = delete;
    AsyncLogger& operator=(const AsyncLogger&) = delete;

    ~AsyncLogger() { shutdown(); }

    void shutdown() {
        if (shutdown_called) return;
        shutdown_called = true;
        running = false;
        cv.notify_all();
        if (worker.joinable()) worker.join();
        if (fout.is_open()) fout.close();
    }

    void log(LogLevel lvl, const string &msg) {
        if (lvl < level.load()) return;
        if (!running) return;
        const char* lvl_str = "";
        switch (lvl) {
            case LogLevel::DEBUG: lvl_str = "DEBUG"; break;
            case LogLevel::INFO:  lvl_str = "INFO";  break;
            case LogLevel::WARN:  lvl_str = "WARN";  break;
            case LogLevel::ERROR: lvl_str = "ERROR"; break;
        }
        string line = "[" + now_time() + "][" + lvl_str + "] " + msg;
        {
            lock_guard<mutex> lock(mtx);
            if (swap_flag) buffer1.push_back(move(line));
            else           buffer2.push_back(move(line));
        }
        cv.notify_one();
    }

    void set_level(LogLevel lvl) {
        level.store(lvl);
        log(LogLevel::INFO, "[AsyncLogger] log level updated");
    }
};

// 不使用全局对象，改为指针，由子进程内创建和销毁
static AsyncLogger* g_logger = nullptr;

inline void glog(LogLevel lvl, const string& msg) {
    if (g_logger) g_logger->log(lvl, msg);
}

class ThreadPool {
private:
    vector<thread> workers;
    queue<function<void()>> tasks;
    mutex queueMutex;
    condition_variable condvar;
    atomic<bool> running{true};
    atomic<size_t> target_thread_num;

public:
    ThreadPool(size_t threadNum) : target_thread_num(threadNum) {
        resize(threadNum);
    }
    ~ThreadPool() { shutdown(); }

    void submit(function<void()> task) {
        lock_guard<mutex> lock(queueMutex);
        if (!running) return;
        tasks.push(move(task));
        condvar.notify_one();
    }

    void resize(size_t newSize) {
        target_thread_num = newSize;
        lock_guard<mutex> lock(queueMutex);
        for (size_t i = workers.size(); i < newSize; ++i)
            workers.emplace_back([this]() { worker_loop(); });
        glog(LogLevel::INFO, "[ThreadPool] resized to " + to_string(newSize) + " threads");
    }

    void shutdown() {
        {
            lock_guard<mutex> lock(queueMutex);
            if (!running) return;
            running = false;
        }
        condvar.notify_all();
        for (auto &t : workers) if (t.joinable()) t.join();
        workers.clear();
    }

    void worker_loop() {
        while (true) {
            function<void()> task;
            {
                unique_lock<mutex> lock(queueMutex);
                condvar.wait(lock, [this]() { return !tasks.empty() || !running; });
                if (!running && tasks.empty()) return;
                if (workers.size() > target_thread_num) return;
                if (!tasks.empty()) { task = move(tasks.front()); tasks.pop(); }
            }
            if (task) {
                try { task(); }
                catch (const exception &e) { glog(LogLevel::ERROR, "[Worker] " + string(e.what())); }
                catch (...) { glog(LogLevel::ERROR, "[Worker] unknown exception"); }
            }
        }
    }
};

int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

class EpollServer {
private:
    int listen_fd = -1;
    int epfd = -1;
    ThreadPool &pool;
    atomic<bool> running{true};
    mutex buffers_mtx;
    unordered_map<int, string> buffers;
    atomic<int> tps{0};
    atomic<int64_t> total_tps{0};

public:
    EpollServer(int port, ThreadPool &tp) : pool(tp) {
        listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        int opt = 1;
        setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);
        bind(listen_fd, (sockaddr *)&addr, sizeof(addr));
        listen(listen_fd, 512);
        set_nonblocking(listen_fd);

        epfd = epoll_create1(0);
        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLET;
        ev.data.fd = listen_fd;
        epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);

        glog(LogLevel::INFO, "[Server] listening on port " + to_string(port));
    }

    void run() {
        vector<epoll_event> events(1024);
        while (running) {
            int n = epoll_wait(epfd, events.data(), events.size(), 500);
            if (n < 0) {
                if (errno == EINTR) { if (!running) break; else continue; }
                if (running) perror("epoll_wait");
                break;
            }
            for (int i = 0; i < n; i++) {
                int fd = events[i].data.fd;
                if (fd == listen_fd) accept_client();
                else handle_client(fd);
            }
        }
        // run() 退出时统一清理，不与 stop() 竞争
        {
            lock_guard<mutex> lock(buffers_mtx);
            for (auto &kv : buffers) {
                epoll_ctl(epfd, EPOLL_CTL_DEL, kv.first, nullptr);
                close(kv.first);
            }
            buffers.clear();
        }
        if (listen_fd >= 0) { close(listen_fd); listen_fd = -1; }
        if (epfd >= 0)      { close(epfd);      epfd = -1; }
        glog(LogLevel::INFO, "[Server] stopped.");
    }

    // stop() 只设标志，资源清理全在 run() 里
    void stop() { running = false; }

    void accept_client() {
        while (true) {
            sockaddr_in client{};
            socklen_t len = sizeof(client);
            int cfd = accept(listen_fd, (sockaddr *)&client, &len);
            if (cfd < 0) { if (errno == EAGAIN) break; continue; }
            set_nonblocking(cfd);
            epoll_event ev{};
            ev.events = EPOLLIN | EPOLLET;
            ev.data.fd = cfd;
            epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &ev);
        }
    }

    void close_client(int fd) {
        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
        close(fd);
        lock_guard<mutex> lock(buffers_mtx);
        buffers.erase(fd);
    }

    void handle_client(int fd) {
        char buf[1024];
        while (true) {
            ssize_t n = read(fd, buf, sizeof(buf));
            if (n > 0) {
                { lock_guard<mutex> lock(buffers_mtx); buffers[fd].append(buf, n); }
                process_buffer(fd);
            } else if (n == 0) {
                close_client(fd); break;
            } else {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                close_client(fd); break;
            }
        }
    }

    void process_buffer(int fd) {
        while (true) {
            string msg;
            {
                lock_guard<mutex> lock(buffers_mtx);
                auto it = buffers.find(fd);
                if (it == buffers.end()) break;
                auto &b = it->second;
                size_t pos = b.find('\n');
                if (pos == string::npos) break;
                msg = b.substr(0, pos);
                b.erase(0, pos + 1);
            }
            tps++;
            total_tps++;
            pool.submit([fd, msg]() {
                stringstream ss; ss << this_thread::get_id();
                glog(LogLevel::DEBUG, "[Worker] thread=" + ss.str() +
                     " fd=" + to_string(fd) + " msg=" + msg);
            });
            string echo = msg + "\n";
            write(fd, echo.c_str(), echo.size());
        }
    }

    int get_tps()           { return tps.exchange(0); }
    int64_t get_total_tps() { return total_tps.load(); }
};

void monitor(EpollServer &server, atomic<bool> &running, Config &cfg) {
    uint64_t lastTotalTime = 0;
    auto lastCheck = chrono::steady_clock::now();
    while (running) {
        this_thread::sleep_for(chrono::milliseconds(cfg.monitor_interval));
        if (!running) break;

        ifstream fstat("/proc/self/stat");
        string tmp; uint64_t utime = 0, stime = 0;
        for (int i = 0; i < 13; i++) fstat >> tmp;
        fstat >> utime >> stime;
        uint64_t totalTime = utime + stime;

        auto now = chrono::steady_clock::now();
        double elapsedSec = chrono::duration<double>(now - lastCheck).count();
        double cpuUsage = 0.0;
        if (lastTotalTime != 0 && elapsedSec > 0)
            cpuUsage = (double)(totalTime - lastTotalTime) / sysconf(_SC_CLK_TCK) / elapsedSec * 100.0;
        lastTotalTime = totalTime; lastCheck = now;

        ifstream fstatus("/proc/self/status");
        string line; size_t memKB = 0;
        while (getline(fstatus, line))
            if (line.find("VmRSS:") != string::npos) {
                stringstream ss(line); string key; ss >> key >> memKB;
            }

        int cur_tps = server.get_tps();
        stringstream ss;
        ss << "[Monitor] CPU=" << cpuUsage << "% Mem=" << memKB/1024.0 << "MB TPS=" << cur_tps;
        glog(LogLevel::INFO, ss.str());

        if      (cpuUsage >= cfg.cpu_error) glog(LogLevel::ERROR, "[ALARM] CPU too high: " + to_string(cpuUsage) + "%");
        else if (cpuUsage >= cfg.cpu_warn)  glog(LogLevel::WARN,  "[ALARM] CPU warning: "  + to_string(cpuUsage) + "%");

        double memMB = memKB/1024.0;
        if      (memMB >= cfg.mem_error) glog(LogLevel::ERROR, "[ALARM] Mem too high: " + to_string(memMB) + "MB");
        else if (memMB >= cfg.mem_warn)  glog(LogLevel::WARN,  "[ALARM] Mem warning: "  + to_string(memMB) + "MB");

        if      (cur_tps >= cfg.tps_error) glog(LogLevel::ERROR, "[ALARM] TPS too high: " + to_string(cur_tps));
        else if (cur_tps >= cfg.tps_warn)  glog(LogLevel::WARN,  "[ALARM] TPS warning: "  + to_string(cur_tps));
    }
}

class ConfigManager {
private:
    string filename;
    Config *cfg;
    atomic<bool> running{true};
    ThreadPool *pool;
    time_t last_mtime{0};
public:
    ConfigManager(const string &file, Config *c, ThreadPool *p)
        : filename(file), cfg(c), pool(p) {}

    void stop() { running = false; }

    void watch_loop() {
        while (running) {
            this_thread::sleep_for(chrono::milliseconds(cfg->monitor_interval));
            if (!running) break;
            struct stat st;
            if (stat(filename.c_str(), &st) == 0 && st.st_mtime != last_mtime) {
                last_mtime = st.st_mtime;
                update_config();
            }
        }
    }

    void update_config() {
        ifstream f(filename);
        if (!f.is_open()) return;
        json j;
        try { f >> j; } catch (...) {
            glog(LogLevel::WARN, "[ConfigManager] failed to parse config");
            return;
        }
        if (j.contains("thread_num")) {
            int n = j["thread_num"];
            if (n != cfg->thread_num) {
                glog(LogLevel::INFO, "[ConfigManager] thread_num: " +
                     to_string(cfg->thread_num) + " -> " + to_string(n));
                pool->resize(n);
                cfg->thread_num = n;
            }
        }
        if (j.contains("log_level") && g_logger) {
            string lvl = j["log_level"];
            LogLevel newLvl = LogLevel::INFO;
            if      (lvl == "DEBUG") newLvl = LogLevel::DEBUG;
            else if (lvl == "WARN")  newLvl = LogLevel::WARN;
            else if (lvl == "ERROR") newLvl = LogLevel::ERROR;
            g_logger->set_level(newLvl);
        }
    }
};

// ---- 子进程信号标志 ----
static atomic<bool> g_child_quit{false};

void child_signal_handler(int) {
    g_child_quit = true;
}

int run_server() {
    // 子进程内创建 logger（栈上局部对象，不是全局，fork 不影响它）
    AsyncLogger logger("server.log", LogLevel::INFO);
    g_logger = &logger;

    signal(SIGINT,  child_signal_handler);
    signal(SIGTERM, child_signal_handler);

    Config cfg;
    ThreadPool pool(cfg.thread_num);
    EpollServer server(cfg.server_port, pool);

    thread server_thread([&]() { server.run(); });

    atomic<bool> monitor_running{true};
    thread mon_thread(monitor, ref(server), ref(monitor_running), ref(cfg));

    ConfigManager cfgMgr("config.json", &cfg, &pool);
    thread cfg_thread([&]() { cfgMgr.watch_loop(); });

    while (!g_child_quit)
        this_thread::sleep_for(chrono::milliseconds(100));

    glog(LogLevel::INFO, "Shutting down...");

    // 正确顺序：cfg → monitor → server(epoll停止+join) → pool → logger
    cfgMgr.stop();
    if (cfg_thread.joinable()) cfg_thread.join();

    monitor_running = false;
    if (mon_thread.joinable()) mon_thread.join();

    server.stop();
    if (server_thread.joinable()) server_thread.join();

    pool.shutdown();

    cout << "[Server] Total requests: " << server.get_total_tps() << endl;
    glog(LogLevel::INFO, "[Server] Total requests: " + to_string(server.get_total_tps()));
    glog(LogLevel::INFO, "Server exited gracefully.");

    g_logger = nullptr;
    logger.shutdown(); // 手动提前关闭，析构时 shutdown_called==true 不会重复执行

    return 0;
}

// ---- 父进程 Watchdog ----
static pid_t g_child_pid = 0;

void watchdog_signal_handler(int sig) {
    if (g_child_pid > 0) kill(g_child_pid, sig);
}

int main() {
    signal(SIGINT,  watchdog_signal_handler);
    signal(SIGTERM, watchdog_signal_handler);

    while (true) {
        pid_t pid = fork();
        if (pid < 0) { perror("fork failed"); return 1; }

        if (pid == 0) {
            // 子进程：重置信号为默认，由 run_server 重新注册
            signal(SIGINT,  SIG_DFL);
            signal(SIGTERM, SIG_DFL);
            g_child_quit = false;
            g_logger = nullptr;
            exit(run_server());
        }

        g_child_pid = pid;
        cout << "[Watchdog] started child pid=" << pid << endl;

        int status = 0;
        cout<<pid<<endl;
        waitpid(pid, &status, 0);
        g_child_pid = 0;

        if (WIFEXITED(status)) {
            int code = WEXITSTATUS(status);
            cout << "[Watchdog] child exited with code " << code << endl;
            if (code == 0) {
                cout << "[Watchdog] normal shutdown." << endl;
                break;
            }
        }

        if (WIFSIGNALED(status)) {
            int sig = WTERMSIG(status);
            cout << "[Watchdog] child killed by signal " << sig << endl;
            // SIGINT/SIGTERM 转发后子进程会优雅退出(code=0)，不走到这里
            // 走到这里说明是真正崩溃（SIGSEGV 等），继续重启
        }

        cout << "[Watchdog] restarting in 2 seconds..." << endl;
        sleep(2);
    }

    return 0;
}