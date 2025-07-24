#include "reactor.hpp"
#include <thread>

#define MAX_PORTS	20

int main(int argc, char **argv)
{
    std::cout << "Usage: " << argv[0] << " <ip> <port> " << std::endl;
    if (argc != 3) {
        std::cerr << "Invalid arguments" << std::endl;
        return 1;
    }

    std::string ip = argv[1];
    int port = std::stoi(argv[2]);

    //int port_count = 10;
    for (int i = 0; i < MAX_PORTS; ++i){
        auto acceptor = std::make_shared<EchoAcceptor>(ip, port + i);
        acceptor->init();
    }

    auto begin = std::chrono::system_clock::now();

    std::thread stats_thread([&begin]{
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(3));
            size_t current_conns = Reactor::get_instance().connection_count();
            size_t total_conns = Reactor::get_instance().total_handlers();
           
	   //static size_t old_conns = 0; 
             	  auto now = std::chrono::system_clock::now();
            auto now_time = std::chrono::system_clock::to_time_t(now);

            //if (current_conns > 0 && (current_conns % 1000) == 0 && current_conns != old_conns) {
             	  //auto now = std::chrono::system_clock::now();
            //    auto duration = now - begin;
          //      auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
	//	begin = now;
		//old_conns = current_conns;
                // 格式化输出
                std::cout << "\n===== Server Statistics =====\n";
             //   std::cout << "Time elapsed: " << ms << "ms\n";
                std::cout << "Time: " << std::put_time(std::localtime(&now_time), "%Y-%m-%d %H:%M:%S") << "\n";
                std::cout << "Total connections: " << total_conns << "\n";
                std::cout << "Current connections: " << current_conns << "\n";
                std::cout << "============================\n";
            //}
        }
    });
    Reactor::get_instance().run();
}
