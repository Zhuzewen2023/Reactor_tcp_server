#include "reactor.hpp"
#include <thread>

int main(int argc, char **argv)
{
    std::cout << "Usage: " << argv[0] << " <ip> <port> " << std::endl;
    if (argc != 3) {
        std::cerr << "Invalid arguments" << std::endl;
        return 1;
    }

    std::string ip = argv[1];
    int port = std::stoi(argv[2]);

    int port_count = 10;
    for (int i = 0; i < port_count; ++i){
        auto acceptor = std::make_shared<HttpAcceptor>(ip, port + i);
        acceptor->init();
    }

    std::thread stats_thread([]{
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            size_t current_conns = Reactor::get_instance().connection_count();
            size_t total_conns = Reactor::get_instance().total_handlers();
            auto now = std::chrono::system_clock::now();
            auto now_time = std::chrono::system_clock::to_time_t(now);

            // 格式化输出
            std::cout << "\n===== Server Statistics =====\n";
            std::cout << "Time: " << std::put_time(std::localtime(&now_time), "%Y-%m-%d %H:%M:%S") << "\n";
            std::cout << "Total connections: " << total_conns << "\n";
            std::cout << "Current connections: " << current_conns << "\n";
            std::cout << "============================\n";
        }
    });
    Reactor::get_instance().run();
}