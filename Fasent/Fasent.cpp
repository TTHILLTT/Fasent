// multithread_file_transfer.cpp
#define _WINSOCK_DEPRECATED_NO_WARNINGS

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iostream>
#include <fstream>
#include <string>
#include <cstdint>
#include <thread>
#include <mutex>

#pragma comment(lib, "ws2_32.lib")

std::mutex cout_mutex;    // 用于保护 std::cout 的打印

// 如果系统已经提供了 htonll/ntohll 函数，那么可重命名自定义实现
#ifndef HAVE_HTONLL
uint64_t myHtonll(uint64_t value) {
    static const int num = 42;
    if (*(const char*)&num == num) { // 小端系统
        uint32_t high_part = htonl((uint32_t)(value >> 32));
        uint32_t low_part = htonl((uint32_t)(value & 0xFFFFFFFFLL));
        return (((uint64_t)low_part) << 32) | high_part;
    }
    else {
        return value;
    }
}

uint64_t myNtohll(uint64_t value) {
    static const int num = 42;
    if (*(const char*)&num == num) { // 小端系统
        uint32_t high_part = ntohl((uint32_t)(value >> 32));
        uint32_t low_part = ntohl((uint32_t)(value & 0xFFFFFFFFLL));
        return (((uint64_t)low_part) << 32) | high_part;
    }
    else {
        return value;
    }
}
#else
// 如果系统已有 htonll/ntohll，可以直接使用或进行宏转换
#define myHtonll(x) htonll(x)
#define myNtohll(x) ntohll(x)
#endif

// 接收指定字节数数据
bool recvAll(SOCKET s, char* buf, int len) {
    int totalReceived = 0;
    while (totalReceived < len) {
        int n = recv(s, buf + totalReceived, len - totalReceived, 0);
        if (n <= 0) return false;
        totalReceived += n;
    }
    return true;
}

// 保证发送所有数据
bool sendAll(SOCKET s, const char* buf, int len) {
    int totalSent = 0;
    while (totalSent < len) {
        int n = send(s, buf + totalSent, len - totalSent, 0);
        if (n == SOCKET_ERROR) return false;
        totalSent += n;
    }
    return true;
}

// 用于处理每个客户端连接的线程函数
void handleClient(SOCKET clientSocket) {
    {
        std::lock_guard<std::mutex> lock(cout_mutex);
        std::cout << "[线程 " << std::this_thread::get_id() << "] 开始处理新客户端连接..." << std::endl;
    }

    // 1. 接收文件名长度（4字节）
    uint32_t nameLen = 0;
    if (!recvAll(clientSocket, (char*)&nameLen, sizeof(nameLen))) {
        std::lock_guard<std::mutex> lock(cout_mutex);
        std::cerr << "[线程 " << std::this_thread::get_id() << "] 接收文件名长度失败" << std::endl;
        closesocket(clientSocket);
        return;
    }
    nameLen = ntohl(nameLen);
    if (nameLen == 0 || nameLen > 1024) {
        std::lock_guard<std::mutex> lock(cout_mutex);
        std::cerr << "[线程 " << std::this_thread::get_id() << "] 文件名长度异常：" << nameLen << std::endl;
        closesocket(clientSocket);
        return;
    }

    // 2. 接收文件名
    char* fileNameBuf = new char[nameLen + 1];
    if (!recvAll(clientSocket, fileNameBuf, nameLen)) {
        std::lock_guard<std::mutex> lock(cout_mutex);
        std::cerr << "[线程 " << std::this_thread::get_id() << "] 接收文件名失败" << std::endl;
        delete[] fileNameBuf;
        closesocket(clientSocket);
        return;
    }
    fileNameBuf[nameLen] = '\0';
    std::string fileName(fileNameBuf);
    delete[] fileNameBuf;
    {
        std::lock_guard<std::mutex> lock(cout_mutex);
        std::cout << "[线程 " << std::this_thread::get_id() << "] 接收到文件名: " << fileName << std::endl;
    }

    // 3. 接收文件大小（8字节）
    uint64_t fileSize = 0;
    if (!recvAll(clientSocket, (char*)&fileSize, sizeof(fileSize))) {
        std::lock_guard<std::mutex> lock(cout_mutex);
        std::cerr << "[线程 " << std::this_thread::get_id() << "] 接收文件大小失败" << std::endl;
        closesocket(clientSocket);
        return;
    }
    fileSize = myNtohll(fileSize);
    {
        std::lock_guard<std::mutex> lock(cout_mutex);
        std::cout << "[线程 " << std::this_thread::get_id() << "] 文件大小: " << fileSize << " 字节" << std::endl;
    }

    // 4. 接收文件内容，并保存到当前目录下
    std::ofstream outFile(fileName, std::ios::binary);
    if (!outFile) {
        std::lock_guard<std::mutex> lock(cout_mutex);
        std::cerr << "[线程 " << std::this_thread::get_id() << "] 无法打开文件写入: " << fileName << std::endl;
        closesocket(clientSocket);
        return;
    }

    const int bufferSize = 4096;
    char buffer[bufferSize];
    uint64_t totalReceived = 0;
    while (totalReceived < fileSize) {
        int bytesToRecv = bufferSize;
        if (fileSize - totalReceived < (uint64_t)bufferSize)
            bytesToRecv = (int)(fileSize - totalReceived);

        int n = recv(clientSocket, buffer, bytesToRecv, 0);
        if (n <= 0) {
            std::lock_guard<std::mutex> lock(cout_mutex);
            std::cerr << "[线程 " << std::this_thread::get_id() << "] 在接收文件内容时出错" << std::endl;
            break;
        }
        outFile.write(buffer, n);
        totalReceived += n;
    }
    outFile.close();
    {
        std::lock_guard<std::mutex> lock(cout_mutex);
        std::cout << "[线程 " << std::this_thread::get_id() << "] 文件接收完毕，共接收 " << totalReceived << " 字节" << std::endl;
    }
    closesocket(clientSocket);
}

// 多线程服务器实现：主线程不断接受连接，为每个连接启动一个线程处理
int runServer(int port) {
    WSADATA wsaData;
    int iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (iResult != 0) {
        std::cerr << "WSAStartup 失败: " << iResult << std::endl;
        return 1;
    }

    SOCKET listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSocket == INVALID_SOCKET) {
        std::cerr << "socket() 失败: " << WSAGetLastError() << std::endl;
        WSACleanup();
        return 1;
    }

    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;  // 绑定所有 IP 地址
    serverAddr.sin_port = htons(port);

    if (bind(listenSocket, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        std::cerr << "bind() 失败: " << WSAGetLastError() << std::endl;
        closesocket(listenSocket);
        WSACleanup();
        return 1;
    }

    if (listen(listenSocket, SOMAXCONN) == SOCKET_ERROR) {
        std::cerr << "listen() 失败: " << WSAGetLastError() << std::endl;
        closesocket(listenSocket);
        WSACleanup();
        return 1;
    }
    std::cout << "服务器已启动，监听端口: " << port << std::endl;

    while (true) {
        SOCKET clientSocket = accept(listenSocket, nullptr, nullptr);
        if (clientSocket == INVALID_SOCKET) {
            std::cerr << "accept() 失败: " << WSAGetLastError() << std::endl;
            continue;
        }
        // 为每个连接创建新线程并分离
        std::thread t(handleClient, clientSocket);
        t.detach();
    }

    // 注意：实际应用中可能需要设置退出条件，然后关闭 listenSocket 和调用 WSACleanup()
    closesocket(listenSocket);
    WSACleanup();
    return 0;
}

int runClient(const char* serverIp, int port, const char* filePath) {
    WSADATA wsaData;
    int iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (iResult != 0) {
        std::cerr << "WSAStartup 失败: " << iResult << std::endl;
        return 1;
    }

    SOCKET connectSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (connectSocket == INVALID_SOCKET) {
        std::cerr << "socket() 失败: " << WSAGetLastError() << std::endl;
        WSACleanup();
        return 1;
    }

    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);

    iResult = inet_pton(AF_INET, serverIp, &serverAddr.sin_addr);
    if (iResult <= 0) {
        std::cerr << "无效的服务器 IP 地址" << std::endl;
        closesocket(connectSocket);
        WSACleanup();
        return 1;
    }

    if (connect(connectSocket, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        std::cerr << "connect() 失败: " << WSAGetLastError() << std::endl;
        closesocket(connectSocket);
        WSACleanup();
        return 1;
    }
    std::cout << "已连接到服务器: " << serverIp << ":" << port << std::endl;

    // 打开文件
    std::ifstream inFile(filePath, std::ios::binary | std::ios::ate);
    if (!inFile) {
        std::cerr << "无法打开文件: " << filePath << std::endl;
        closesocket(connectSocket);
        WSACleanup();
        return 1;
    }
    std::streamsize fileSize = inFile.tellg();
    inFile.seekg(0, std::ios::beg);

    // 简单提取文件名（路径最后部分）
    std::string filePathStr(filePath);
    size_t pos = filePathStr.find_last_of("\\/");
    std::string fileName = (pos == std::string::npos) ? filePathStr : filePathStr.substr(pos + 1);

    // 1. 发送文件名长度（4字节）
    uint32_t nameLen = (uint32_t)fileName.length();
    uint32_t netNameLen = htonl(nameLen);
    if (!sendAll(connectSocket, (char*)&netNameLen, sizeof(netNameLen))) {
        std::cerr << "发送文件名长度失败" << std::endl;
        closesocket(connectSocket);
        WSACleanup();
        return 1;
    }

    // 2. 发送文件名
    if (!sendAll(connectSocket, fileName.c_str(), nameLen)) {
        std::cerr << "发送文件名失败" << std::endl;
        closesocket(connectSocket);
        WSACleanup();
        return 1;
    }
    std::cout << "发送文件名: " << fileName << std::endl;

    // 3. 发送文件大小（8字节）
    uint64_t netFileSize = myHtonll((uint64_t)fileSize);
    if (!sendAll(connectSocket, (char*)&netFileSize, sizeof(netFileSize))) {
        std::cerr << "发送文件大小失败" << std::endl;
        closesocket(connectSocket);
        WSACleanup();
        return 1;
    }
    std::cout << "文件大小: " << fileSize << " 字节" << std::endl;

    // 4. 发送文件内容
    const int bufferSize = 4096;
    char buffer[bufferSize];
    uint64_t totalSent = 0;
    while (!inFile.eof() && totalSent < (uint64_t)fileSize) {
        inFile.read(buffer, bufferSize);
        std::streamsize bytesRead = inFile.gcount();
        if (bytesRead <= 0) break;
        if (!sendAll(connectSocket, buffer, (int)bytesRead)) {
            std::cerr << "发送文件内容失败" << std::endl;
            break;
        }
        totalSent += bytesRead;
    }
    inFile.close();
    std::cout << "文件发送完毕，共发送 " << totalSent << " 字节" << std::endl;

    closesocket(connectSocket);
    WSACleanup();
    return 0;
}

// 根据命令行参数选择服务器或客户端模式
int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "使用方法：\n"
            << "  作为服务器: " << argv[0] << " server <port>\n"
            << "  作为客户端: " << argv[0] << " client <server_ip> <port> <file_path>\n";
        return 1;
    }

    std::string mode = argv[1];
    if (mode == "server") {
        int port = std::stoi(argv[2]);
        return runServer(port);
    }
    else if (mode == "client") {
        if (argc < 5) {
            std::cerr << "客户端模式缺少参数\n";
            return 1;
        }
        const char* serverIp = argv[2];
        int port = std::stoi(argv[3]);
        const char* filePath = argv[4];
        return runClient(serverIp, port, filePath);
    }
    else {
        std::cerr << "未知模式: " << mode << "\n";
        return 1;
    }
}
