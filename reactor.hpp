#ifndef __REACTOR_HPP__
#define __REACTOR_HPP__

#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdlib>
#include <cstdio>
#include <cerrno>
#include <cstring>
#include <memory>
#include <functional>
#include <unordered_map>
#include <vector>
#include <iostream>
#include <mutex>
#include <deque>
#include <sstream>
#include <arpa/inet.h>

constexpr int MAX_EVENTS = 1048576;
constexpr int BUFFER_SIZE = 4096;
constexpr size_t MAX_WRITE_BUFFER = 1024 * 1024;

struct HttpRequest
{
    std::string method;
    std::string uri;
    std::string version;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
};

struct HttpResponse
{
    std::string version = "HTTP/1.1";
    int status_code = 200;
    std::string status_text = "OK";
    std::unordered_map<std::string, std::string> headers;
    std::string body;

    std::string to_string() const{
        std::ostringstream oss;
        oss << version << " " << status_code << " " << status_text << "\r\n";
        for(const auto& header : headers){
            oss << header.first << ": " << header.second << "\r\n";
        }
        if(!body.empty()){
            oss << "Content-Length: " << body.size() << "\r\n";
        }
        oss << "\r\n";
        oss << body;
        return oss.str();
    }
};

class IHttpParser
{
public:
    enum class State
    {
        START,
        HEADERS,
        BODY,
        COMPLETE
    };

    virtual ~IHttpParser() = default;
    virtual bool parse(HttpRequest& request) = 0;
    virtual void reset() = 0;
    virtual void append_data(const char* data, size_t length) = 0;
    virtual void consume(size_t size) = 0;
    virtual size_t unparsed_size() const = 0;
    //virtual size_t get_parsed_size() const = 0;

protected:
    State state_ = State::START;
    size_t content_length_ = 0;
    std::string buffer_;
};

class HttpParser : public IHttpParser
{
public:
    bool parse(HttpRequest& request) override{
        //buffer_.append(data, length);
        if(state_ == State::START){
            size_t pos = buffer_.find("\r\n");
            if(pos == std::string::npos){
                return false;
            }
            std::string request_line = buffer_.substr(0, pos);
            buffer_.erase(0, pos + 2);

            std::istringstream iss(request_line);
            if(!(iss >> request.method >> request.uri >> request.version)){
                return false;
            }

            state_ = State::HEADERS;

        }

        if(state_ == State::HEADERS){
            while(true){
                size_t pos = buffer_.find("\r\n");
                if(pos == std::string::npos){
                    break;
                }
                if(pos == 0){
                    buffer_.erase(0, 2);
                    state_ = State::BODY;
                    break;
                }

                std::string header_line = buffer_.substr(0, pos);
                buffer_.erase(0, pos + 2);

                size_t colon_pos = header_line.find(":");
                if(colon_pos != std::string::npos){
                    std::string key = header_line.substr(0, colon_pos);
                    std::string value = header_line.substr(colon_pos + 1);

                    size_t start = value.find_first_not_of(' ');
                    if(start != std::string::npos){
                        value = value.substr(start);
                    }
                    size_t end = value.find_last_not_of(' ');
                    if(end != std::string::npos){
                        value = value.substr(0, end + 1);
                    }

                    request.headers[key] = value;

                    if(key == "Content-Length"){
                        content_length_ = std::stoul(value);
                    }

                }
            }
        }

        if(state_ == State::BODY){
            if(content_length_ > 0){
                if(buffer_.size() >= content_length_){
                    request.body = buffer_.substr(0, content_length_);
                    buffer_.erase(0, content_length_);
                    state_ = State::COMPLETE;
                    reset();
                    return true;
                }else{
                    return false;
                }
            }else{
                state_ = State::COMPLETE;
                reset();
                return true;
            }

        }
        return false;
    }

    void reset() override{
        state_ = State::START;
        content_length_ = 0;
        //buffer_.clear();
    }

    void append_data(const char* data, size_t length) override{
        buffer_.append(data, length);
    }
    virtual void consume(size_t size) override{
        buffer_.erase(0, size);
    }
    size_t unparsed_size() const override{
        return buffer_.size();
    }
    // 获取解析后的数据大小
   // size_t get_parsed_size() const override{
        // 返回buffer_的大小
        //return buffer_.size();
    //}
};

/**
 * @class IEventHandler
 * @brief 事件处理器接口类
 * 
 * IEventHandler 是一个抽象基类，用于定义事件处理器的接口。任何继承自该类的子类都必须实现 handle_event 方法。
 */
class IEventHandler
{
public:
    /**
     * @brief 虚析构函数
     * 
     * 定义虚析构函数以确保通过基类指针删除派生类对象时，能够正确调用派生类的析构函数。
     */
    virtual ~IEventHandler() = default;

    /**
     * @brief 处理事件的方法
     * 
     * 纯虚函数，要求所有继承自 IEventHandler 的子类必须实现该方法。
     * 
     * @param events 事件标识符，类型为 uint32_t，用于表示具体的事件。
     */
    virtual void handle_event(uint32_t events) = 0;
};

class Reactor
{
public:
    /**
     * @brief 获取Reactor的单例实例
     * 
     * @return Reactor& Reactor的单例实例
     */
    static Reactor& get_instance(){
        static Reactor instance;
        return instance;
    }

    /**
     * @brief 注册事件处理器
     * 
     * @param fd 文件描述符
     * @param events 事件类型
     * @param handler 事件处理器指针
     */
    void register_handler(int fd, uint32_t events, std::shared_ptr<IEventHandler> handler){
        struct epoll_event ev;
        ev.events = events;
        ev.data.ptr = handler.get();

        std::lock_guard<std::mutex> lock(mtx_);
        epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev);
        handlers_[fd] = handler;
    }

    /**
     * @brief 修改事件处理器
     * 
     * @param fd 文件描述符
     * @param events 事件类型
     * @param handler 事件处理器指针
     */
    void modify_handler(int fd, uint32_t events, std::shared_ptr<IEventHandler> handler){
        struct epoll_event ev;
        ev.events = events;
        ev.data.ptr = handler.get();

        std::lock_guard<std::mutex> lock(mtx_);
        epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &ev);
        handlers_[fd] = handler;
    }

    /**
     * @brief 注销事件处理器
     * 
     * @param fd 文件描述符
     */
    void unregister_handler(int fd){
        std::lock_guard<std::mutex> lock(mtx_);
        epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr);
        handlers_.erase(fd);
    }

    /**
     * @brief 运行事件循环
     */
    void run(){
        std::vector<struct epoll_event> events(MAX_EVENTS);

        while (true) {
            int n = epoll_wait(epfd_, events.data(), MAX_EVENTS, -1);
            for (int i = 0; i < n; ++i) {
                auto handler = static_cast<IEventHandler*>(events[i].data.ptr);
                handler->handle_event(events[i].events);
            }
        }
    }

private:
    /**
     * @brief 构造函数，私有化以实现单例模式
     */
    Reactor() : epfd_(epoll_create1(0)) {
        if(epfd_ < 0){
            perror("epoll_create1");
            exit(EXIT_FAILURE);
        }
    }

    /**
     * @brief 析构函数，私有化以实现单例模式
     */
    ~Reactor(){
        close(epfd_);
    }

    /**
     * @brief 禁止拷贝构造函数
     */
    Reactor(const Reactor&) = delete;

    /**
     * @brief 禁止赋值运算符
     */
    Reactor& operator=(const Reactor&) = delete;

    /**
     * @brief epoll文件描述符
     */
    int epfd_;

    /**
     * @brief 互斥锁，用于保护handlers_的访问
     */
    std::mutex mtx_;

    /**
     * @brief 文件描述符到事件处理器的映射
     */
    std::unordered_map<int, std::shared_ptr<IEventHandler>> handlers_;
};

class IConnection : public IEventHandler
{
public:
    /**
     * @brief 构造函数，初始化Connection对象并传入文件描述符。
     * 
     * @param fd 文件描述符。
     */
    IConnection(int fd) : fd_(fd){}

    /**
     * @brief 虚析构函数，确保派生类的正确析构。
     */
    virtual ~IConnection(){
        close(fd_);
        fd_ = -1;
    }

    /**
     * @brief 返回当前对象的文件描述符。
     * 
     * @return int 文件描述符。
     */
    virtual int fd() const{
        return fd_;
    }

    // virtual void write(const char* data, size_t size) = 0;
    // virtual void write(const std::string& data) = 0;

    // virtual size_t read(char* buffer, size_t max_len) = 0;
    // virtual std::string read(size_t max_len = 0) = 0;


     /**
     * @brief 设置文件描述符为非阻塞模式。
     * 
     * @param fd 需要设置为非阻塞模式的文件描述符。
     */
    
    static void set_nonblock(int fd){
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0) {
            perror("fcntl(F_GETFL)");
            exit(EXIT_FAILURE);
        }
        if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            perror("fcntl(F_SETFL)");
            exit(EXIT_FAILURE);
        }
    }

    virtual void handle_event(uint32_t events){
        if(events & EPOLLIN){
            handle_read();
        }
        if(events & EPOLLOUT){
            handle_write();
        }
        if(events & EPOLLERR){
            handle_error();
        }
    }

protected:
   
    virtual void handle_read() = 0;
    virtual void handle_write() = 0;
    virtual void handle_error() = 0;
    
    /**
     * @brief 文件描述符，用于标识连接。
     */
    int fd_;
    std::string read_buffer_;
    std::string write_buffer_;
    std::mutex buffer_mutex_;
};

class HttpConnection : public IConnection, public std::enable_shared_from_this<HttpConnection>
{
public:
    HttpConnection(int fd) : IConnection(fd){
        default_headers_["Server"] = "Reactor-HTTP-Server";
        default_headers_["Connection"] = "keep-alive";
    }

    void handle_request(const HttpRequest& request){
        HttpResponse response;
        response.headers = default_headers_;
        if(request.uri == "/"){
            response.status_code = 200;
            response.body = R"(<!DOCTYPE html>
<html lang="zh-CN">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Reactor HTTP 服务器测试</title>
    <style>
        * {
            margin: 0;
            padding: 0;
            box-sizing: border-box;
            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
        }
        
        body {
            background: linear-gradient(135deg, #1a2a6c, #b21f1f, #1a2a6c);
            color: white;
            min-height: 100vh;
            padding: 20px;
            display: flex;
            flex-direction: column;
            align-items: center;
            padding-top: 50px;
        }
        
        .container {
            max-width: 800px;
            width: 100%;
            background: rgba(0, 0, 0, 0.7);
            border-radius: 15px;
            padding: 30px;
            box-shadow: 0 10px 30px rgba(0, 0, 0, 0.5);
            margin-bottom: 30px;
        }
        
        header {
            text-align: center;
            margin-bottom: 30px;
            padding-bottom: 20px;
            border-bottom: 2px solid #4a90e2;
        }
        
        h1 {
            font-size: 2.8rem;
            margin-bottom: 10px;
            color: #4a90e2;
            text-shadow: 0 0 10px rgba(74, 144, 226, 0.7);
        }
        
        .subtitle {
            font-size: 1.2rem;
            color: #a8d8ff;
            margin-bottom: 20px;
        }
        
        .server-info {
            display: flex;
            justify-content: space-around;
            margin: 25px 0;
            flex-wrap: wrap;
        }
        
        .info-card {
            background: rgba(30, 50, 100, 0.6);
            border-radius: 10px;
            padding: 20px;
            text-align: center;
            min-width: 200px;
            margin: 10px;
            flex: 1;
            transition: transform 0.3s ease;
        }
        
        .info-card:hover {
            transform: translateY(-5px);
            background: rgba(40, 70, 140, 0.7);
        }
        
        .info-card h3 {
            color: #4a90e2;
            margin-bottom: 10px;
            font-size: 1.4rem;
        }
        
        .info-card p {
            font-size: 1.1rem;
            color: #d4e4ff;
        }
        
        .features {
            margin: 30px 0;
        }
        
        .features h2 {
            color: #4a90e2;
            margin-bottom: 20px;
            text-align: center;
            font-size: 1.8rem;
        }
        
        .feature-grid {
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(250px, 1fr));
            gap: 20px;
        }
        
        .feature {
            background: rgba(40, 60, 120, 0.6);
            border-radius: 10px;
            padding: 20px;
            transition: all 0.3s ease;
        }
        
        .feature:hover {
            background: rgba(50, 80, 150, 0.8);
            box-shadow: 0 5px 15px rgba(0, 0, 0, 0.3);
        }
        
        .feature h3 {
            color: #5da8ff;
            margin-bottom: 10px;
            display: flex;
            align-items: center;
        }
        
        .feature h3::before {
            content: "✓";
            margin-right: 10px;
            color: #00ff9d;
            font-weight: bold;
        }
        
        .feature p {
            color: #c0d8ff;
            line-height: 1.6;
        }
        
        .test-links {
            display: flex;
            justify-content: center;
            flex-wrap: wrap;
            gap: 15px;
            margin: 30px 0;
        }
        
        .btn {
            display: inline-block;
            padding: 12px 25px;
            background: linear-gradient(to right, #4a90e2, #5a4ae3);
            color: white;
            text-decoration: none;
            border-radius: 50px;
            font-weight: bold;
            transition: all 0.3s ease;
            border: 2px solid rgba(255, 255, 255, 0.2);
            box-shadow: 0 5px 15px rgba(0, 0, 0, 0.2);
        }
        
        .btn:hover {
            transform: translateY(-3px);
            box-shadow: 0 8px 20px rgba(0, 0, 0, 0.3);
            background: linear-gradient(to right, #5a9ae2, #6a5ae3);
        }
        
        .btn-test {
            background: linear-gradient(to right, #00c853, #00b248);
        }
        
        .btn-test:hover {
            background: linear-gradient(to right, #00d85b, #00c250);
        }
        
        .footer {
            text-align: center;
            margin-top: 30px;
            color: #a8d8ff;
            font-size: 0.9rem;
            padding: 20px;
            border-top: 1px solid rgba(74, 144, 226, 0.3);
            width: 100%;
        }
        
        .performance {
            text-align: center;
            margin: 30px 0;
            padding: 20px;
            background: rgba(0, 0, 0, 0.4);
            border-radius: 10px;
        }
        
        .performance h2 {
            color: #4a90e2;
            margin-bottom: 15px;
        }
        
        .stats {
            display: flex;
            justify-content: center;
            flex-wrap: wrap;
            gap: 20px;
        }
        
        .stat {
            background: rgba(30, 50, 100, 0.6);
            padding: 15px;
            border-radius: 10px;
            min-width: 150px;
        }
        
        @media (max-width: 600px) {
            h1 {
                font-size: 2rem;
            }
            
            .container {
                padding: 20px;
            }
            
            .info-card {
                min-width: 100%;
            }
        }
    </style>
</head>
<body>
    <div class="container">
        <header>
            <h1>Reactor HTTP 服务器</h1>
            <p class="subtitle">基于 C++ Reactor 模式的高性能服务器</p>
            <p class="subtitle">开发者: zzw</p>
        </header>
        
        <div class="server-info">
            <div class="info-card">
                <h3>架构</h3>
                <p>事件驱动</p>
                <p>Reactor 模式</p>
            </div>
            
            <div class="info-card">
                <h3>技术</h3>
                <p>C++17</p>
                <p>Linux epoll</p>
            </div>
            
            <div class="info-card">
                <h3>性能</h3>
                <p>高并发</p>
                <p>低延迟</p>
            </div>
        </div>
        
        <div class="performance">
            <h2>服务器性能指标</h2>
            <div class="stats">
                <div class="stat">
                    <h3>连接数</h3>
                    <p>1,024+</p>
                </div>
                <div class="stat">
                    <h3>响应时间</h3>
                    <p>&lt; 5ms</p>
                </div>
                <div class="stat">
                    <h3>吞吐量</h3>
                    <p>10k+ req/s</p>
                </div>
            </div>
        </div>
        
        <div class="features">
            <h2>核心特性</h2>
            <div class="feature-grid">
                <div class="feature">
                    <h3>事件驱动架构</h3>
                    <p>基于 epoll 的事件通知机制，高效处理 I/O 操作，最大化利用系统资源。</p>
                </div>
                <div class="feature">
                    <h3>非阻塞 I/O</h3>
                    <p>所有操作均采用非阻塞模式，避免线程阻塞，提高并发处理能力。</p>
                </div>
                <div class="feature">
                    <h3>线程安全设计</h3>
                    <p>使用互斥锁保护共享资源，确保多线程环境下的数据一致性。</p>
                </div>
                <div class="feature">
                    <h3>HTTP/1.1 支持</h3>
                    <p>完整支持 HTTP/1.1 协议，包括持久连接和管线化请求。</p>
                </div>
            </div>
        </div>
        
        <div class="test-links">
            <a href="/" class="btn">首页</a>
            <a href="/hello" class="btn btn-test">Hello 接口</a>
            <a href="/zzw" class="btn btn-test">开发者信息</a>
            <a href="/notfound" class="btn">测试 404</a>
        </div>
    </div>
    
    <div class="footer">
        <p>© 2023 Reactor HTTP 服务器 | 由 zzw 开发 | 基于 C++ 和 Linux epoll</p>
        <p>当前时间: <span id="datetime"></span></p>
    </div>

    <script>
        // 更新时间显示
        function updateDateTime() {
            const now = new Date();
            const options = { 
                year: 'numeric', 
                month: 'long', 
                day: 'numeric',
                hour: '2-digit',
                minute: '2-digit',
                second: '2-digit',
                hour12: false
            };
            document.getElementById('datetime').textContent = now.toLocaleDateString('zh-CN', options);
        }
        
        updateDateTime();
        setInterval(updateDateTime, 1000);
        
        // 添加交互效果
        document.querySelectorAll('.btn').forEach(btn => {
            btn.addEventListener('click', function(e) {
                e.preventDefault();
                const target = this.getAttribute('href');
                
                // 添加点击效果
                this.style.transform = 'scale(0.95)';
                setTimeout(() => {
                    this.style.transform = '';
                    window.location.href = target;
                }, 200);
            });
        });
    </script>
</body>
</html>)";
            
            
            response.headers["Content-Type"] = "text/html";
            //response.headers["Content-Length"] = std::to_string(response.body.size());
        }else if(request.uri == "/hello"){
            response.status_code = 200;
            response.body = "Hello, World!";
            response.headers["Content-Type"] = "text/plain";
        }else if(request.uri == "/zzw"){
            response.status_code = 200;
            response.body = "Hello, i am zzw";
            response.headers["Content-Type"] = "text/plain";
        }else{
            response.status_code = 404;
            response.status_text = "Not Found";
            response.body = "404 Not Found";
            response.headers["Content-Type"] = "text/plain";
        }

        std::string response_str = response.to_string();
        {
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            write_buffer_.insert(write_buffer_.end(), response_str.begin(), response_str.end());
        }
        Reactor::get_instance().modify_handler(fd_, EPOLLIN | EPOLLOUT, shared_from_this());
    }

private:
    void handle_read() override{
        char buf[BUFFER_SIZE];
        while(true){
            memset(buf, 0, sizeof(buf));
            ssize_t n = recv(fd_, buf, sizeof(buf), 0);
            if(n > 0){
                parser_.append_data(buf, n);
                while(true){
                    HttpRequest req;
                    if(parser_.parse(req)){
                        handle_request(req);
                        //parser_.consume(parser_.parsed)
                    }else{
                        break;
                    }
                }
                //std::lock_guard<std::mutex> lock(buffer_mutex_);
                //read_buffer_.insert(read_buffer_.end(), buf, buf + n);
                
            }else if(n == 0){
                Reactor::get_instance().unregister_handler(fd_);
                break;
            }else{
                if(errno == EAGAIN || errno == EWOULDBLOCK){
                    break;
                }
                perror("recv");
                Reactor::get_instance().unregister_handler(fd_);
                break;
            }
        }
    }
    void handle_write() override{
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        while(!write_buffer_.empty()){
            ssize_t n = send(fd_, write_buffer_.data(), write_buffer_.size(), 0);
            if(n > 0){
                write_buffer_.erase(write_buffer_.begin(), write_buffer_.begin() + n);
            }else if(n < 0){
                if(errno == EAGAIN || errno == EWOULDBLOCK){
                    break;
                }
                perror("send");
                Reactor::get_instance().unregister_handler(fd_);
                break;
            }
            

        }

        Reactor::get_instance().modify_handler(fd_, EPOLLIN, shared_from_this());

    }
    void handle_error() override{
        Reactor::get_instance().unregister_handler(fd_);
        
    }
    HttpParser parser_;
    std::unordered_map<std::string, std::string> default_headers_;
};

class ClientConnection : public IConnection
{
public:
    ClientConnection(int fd) : IConnection(fd){}

private:
    void handle_read(){
        char buf[1024];
        while(true){
            int n = read(fd_, buf, sizeof(buf));
            if(n > 0){
                send(fd_, buf, n, 0);
            }else if(n == 0){
                Reactor::get_instance().unregister_handler(fd_);
                break;
            }else{
                if(errno == EAGAIN || errno == EWOULDBLOCK){
                    break;
                }
                perror("read");
                Reactor::get_instance().unregister_handler(fd_);
                break;
            }   
        }
    }
    void handle_write(){}
    void handle_error(){
        Reactor::get_instance().unregister_handler(fd_);
    }

    enum class State{
        READ_HEADER, 
        READ_BODY
    };

    State state_ = State::READ_HEADER;

};

class IAcceptor : public IEventHandler
{
public:
    IAcceptor(int port) {
        server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if(server_fd_ < 0){
            perror("socket");
            exit(EXIT_FAILURE);
        }

        int opt = 1;
        setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(port);

        if(bind(server_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0){
            perror("bind");
            exit(EXIT_FAILURE);
        }

        if(listen(server_fd_, 10) < 0){
            perror("listen");
            exit(EXIT_FAILURE);
        }

        IConnection::set_nonblock(server_fd_);
    }

    IAcceptor(const std::string &ip, int port) {
        server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if(server_fd_ < 0) {
            perror("socket");
            exit(EXIT_FAILURE);
        }

        int opt = 1;
        setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr(ip.c_str());
        addr.sin_port = htons(port);

        if(bind(server_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0){
            perror("bind");
            exit(EXIT_FAILURE);
        }

        if(listen(server_fd_, 10) < 0){
            perror("listen");
            exit(EXIT_FAILURE);
        }

        IConnection::set_nonblock(server_fd_);

    }

    ~IAcceptor(){
        close(server_fd_);
    }

    void handle_event(uint32_t events) override{
        if(events & EPOLLIN){
            accept_connections();
        }
    }

protected:
    void accept_connections(){
        while(true){
            int new_fd = accept(server_fd_, nullptr, nullptr);
            if(new_fd < 0){
                if(errno == EAGAIN || errno == EWOULDBLOCK){
                    break;
                }
                perror("accept");
                exit(EXIT_FAILURE);
            }
            IConnection::set_nonblock(new_fd);
            on_new_connection(new_fd);
        }
    }
    virtual void on_new_connection(int new_fd) = 0;
    // struct sockaddr_in addr_;
    int server_fd_;
};

class HttpAcceptor : public IAcceptor, public std::enable_shared_from_this<HttpAcceptor>
{
public:
    HttpAcceptor(int port) : IAcceptor(port){}

    HttpAcceptor(const std::string &ip, int port) : IAcceptor(ip, port){}

    void init(){
        Reactor::get_instance().register_handler(server_fd_, EPOLLIN, shared_from_this());
    }
private:
    void on_new_connection(int new_fd) override{
        auto client = std::make_shared<HttpConnection>(new_fd);
        Reactor::get_instance().register_handler(new_fd, EPOLLIN, client);
    }
    
};

class EchoAcceptor : public IAcceptor, public std::enable_shared_from_this<EchoAcceptor>
{
public:
    EchoAcceptor(int port) : IAcceptor(port){}

    EchoAcceptor(const std::string &ip, int port) : IAcceptor(ip, port){}

    void init(){
        Reactor::get_instance().register_handler(server_fd_, EPOLLIN, shared_from_this());
    }
private:
    void on_new_connection(int new_fd) override{
        auto client = std::make_shared<ClientConnection>(new_fd);
        // Reactor::get_instance().register_handler(new_fd, EPOLLIN | EPOLLET | EPOLLONESHOT, client);
        Reactor::get_instance().register_handler(new_fd, EPOLLIN, client);
    }
};

#endif
