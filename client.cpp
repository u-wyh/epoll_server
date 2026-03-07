#include<arpa/inet.h>
#include<unistd.h>
#include<iostream>
#include<thread>
#include<vector>
#include<atomic>
#include<chrono>
#include<csignal>

using namespace std;

atomic<long long> g_send{0};
atomic<long long> g_received{0};
atomic<bool> g_running{true};

void signal_handler(int){
    g_running = false;
}

void client_worker(const string &ip, int port, int id, int message_count, int time){
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);

    if(connect(sock, (sockaddr*)&server_addr, sizeof(server_addr)) < 0){
        perror("connect error.");
        close(sock);
        return;
    }

    for(int i = 0; i < message_count && g_running; i++){
        this_thread::sleep_for(chrono::milliseconds(time));
        string msg = "msg " + to_string(id) + ":" + to_string(i) + "\n";
        send(sock, msg.c_str(), msg.size(), 0);
        g_send++;

        char buf[1024];
        ssize_t n = recv(sock, buf, sizeof(buf), 0);
        if(n > 0){
            g_received++;
        }
    }
    close(sock);
}

int main(){
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    int threads = 500;
    int message_count = 2000;
    int time = 2;

    vector<thread> workers;
    for(int i = 1; i <= threads; i++){
        workers.emplace_back(client_worker, "127.0.0.1", 8080, i, message_count, time);
    }

    thread monitor([&](){
        long long last_send = 0;
        int t = 0;
        while(g_running){
            this_thread::sleep_for(chrono::seconds(1));
            long long cur_send = g_send.load();
            long long cur_recv = g_received.load();
            cout << "[Client Monitor]: " << ++t
                 << " send=" << cur_send
                 << " received=" << cur_recv
                 << " TPS=" << (cur_send - last_send) << endl;
            last_send = cur_send;
        }
    });

    for(auto &t : workers){
        if(t.joinable())
            t.join();
    }

    g_running = false;
    if(monitor.joinable())
        monitor.join();

    return 0;
}
