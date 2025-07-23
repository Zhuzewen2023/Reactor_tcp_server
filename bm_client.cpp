#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdlib>
#include <cstdio>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <random>
#include <algorithm>

constexpr int MAX_EVENTS = 10000;
constexpr int BUFFER_SIZE = 4096;
constexpr int MAX_CONNECTIONS_PER_THREAD = 50000;


class LoadTester
{
public:
    LoadTester(const std::string& ip, int port, int connections, int threads)
        : ip_(ip), port_(port), total_connections_(connections), num_threads_(threads), stop_(false)
    {
        connections_per_thread_ = std::min(connections / threads, MAX_CONNECTIONS_PER_THREAD);
    }

    void start() {
        std::vector<std::thread> workers;
        std::atomic<int> established(0);

        auto start_time = std::chrono::steady_clock::now();

        for (int i = 0; i < num_threads_; ++i) {
            workers.emplace_back([this, &established](){
                worker_thread(established);
            });
        }

        while (established < total_connections_ && !stop_) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            auto now = std::chrono::steady_clock::now();

            auto elapsed = 
            std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time).count();
            
            std::cout   << "已建立连接: " << established << "/" << total_connections_ 
                        << " (" << (100.0 * established / total_connections_) << "%)" 
                        << "耗时: " << elapsed << "ms" << std::endl;
        }

        auto end_time = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
        std::cout << "\n==========所有连接建立完成==========" << std::endl;
        std::cout << "总连接数: " << total_connections_ << std:: endl;
        std::cout << "线程数: " << num_threads_ << std::endl;
        std::cout << "耗时: " << duration << "毫秒" << std::endl;
        std::cout << "平均建立连接速率" << (total_connections_ * 1000 / duration) << " 连接/秒" << std::endl;

        stop_ = true;

        for (auto& t : workers) {
            t.join();
        }

        std::cout << "===========测试结束===========" << std::endl;
    }
private:
    void worker_thread(std::atomic<int>& established) {
        int epoll_fd = epoll_create1(0);
        if (epoll_fd < 0) {
            perror("epoll_create1");
            return;
        }

        std::vector<int> sockets;
        std::vector<time_t> last_activity;
        std::vector<bool> is_connected;

        for (int i = 0; i < connections_per_thread_; ++i) {
            int sock = create_socket();

            if (sock >= 0) {
                sockets.push_back(sock);
                last_activity.push_back(time(NULL));
                is_connected.push_back(false);
                add_epoll_event(epoll_fd, sock, EPOLLOUT);
            }
        }

        struct epoll_event events[MAX_EVENTS];
        bool wait_for_signal = true;

        while (!stop_) {
            int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, 1000);
            time_t now = time(NULL);

            for (int i = 0; i < nfds; ++i) {
                int fd = events[i].data.fd;
                auto it = std::find(sockets.begin(), sockets.end(), fd);
                if (it == sockets.end()) continue;

                int index = std::distance(sockets.begin(), it);
                
                if (events[i].events & EPOLLOUT) {
                    int error = 0;
                    socklen_t len = sizeof(error);
                    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) < 0){
                        perror("getsockopt failed");
                        close(fd);
                        sockets[index] = -1;
                        continue;
                    }
                    if (error) {
                        fprintf(stderr, "建立连接失败: %s\n", strerror(error));
                        close(fd);
                        sockets[index] = -1;
                        continue;
                    }

                    is_connected[index] = true;
                    established++;
                    events[i].events = EPOLLIN | EPOLLET;
                    if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &events[i]) == -1) {
                        perror("epoll ctl");
                    }
                } else if (events[i].events & EPOLLIN) {
                    char buf[BUFFER_SIZE];
                    ssize_t n = recv(fd, buf, sizeof(buf), 0);
                    if (n > 0) {
                        last_activity[index] = now;
                    } else if (n == 0 || (n < 0 && errno != EAGAIN)) {
                        close(fd);
                        sockets[index] = -1;
                    }
                }
            }
        }

        for (int sock : sockets) {
            if (sock >= 0) close(sock);
        }
        close(epoll_fd);
    }

    int create_socket() {
        int sock = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (sock < 0) {
            return -1;
        }
        int port = port_ + current_index_;
        current_index_ = (current_index_ + 1) % 10;
        struct sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        
        if (inet_pton(AF_INET, ip_.c_str(), &addr.sin_addr) <= 0) {
            close(sock);
            return -1;
        }

        int ret = connect(sock, (struct sockaddr*)&addr, sizeof(addr));
        if (ret < 0) {
            if (errno != EINPROGRESS) {
                perror("connec failed");
                close(sock);
                return -1;
            }
        }
        return sock;
    }

    void add_epoll_event(int epoll_fd, int fd, uint32_t events) {
        struct epoll_event ev;
        ev.events = events;
        ev.data.fd = fd;
        epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev);
    }

    std::string ip_;
    int port_;
    int total_connections_;
    int num_threads_;
    int connections_per_thread_;
    int current_index_ = 0;
    std::atomic<bool> stop_{false};
};

int main(int argc, char **argv)
{
    if (argc < 5) {
        std::cerr << "Usage: " << argv[0] << " <server_ip> <server_port> <connections> <threads>\n"
                  << "Example: " << argv[0] << " 192.168.1.100 8080 1000000 20\n";
        return 1;
    }

    std::string ip = argv[1];
    int port = std::stoi(argv[2]);
    int connections = std::stoi(argv[3]);
    int threads = std::atoi(argv[4]);

    
    LoadTester tester(ip, port, connections, threads);
    tester.start();
    return 0;
}