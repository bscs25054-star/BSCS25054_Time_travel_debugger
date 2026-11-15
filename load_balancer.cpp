// ======================= TIME-TRAVEL DEBUGGER - LOAD BALANCER TEMPLATE =======================

// What load balancer does:
//   1. Many clients can connect at the same time. Each one sends one string (the source code). 
//   2. Clients wait in a queue (first come, first served).
//   3. For each client, in order: make sure the server is running (start it with system() if not),
//      pass the string to the server, pass the server's reply back to the client.
//
// It does not read or understand the messages. It only moves bytes.
// The only messages it writes itself are errors ("server busy", "server failed to start").
//
// Two threads:
//   acceptor thread (acceptLoop) -> takes new clients and puts them in the queue. It never waits for the server.
//   main thread (dispatcherLoop) -> takes ONE client out of the queue and serves it completely, then the next.
//
// How the server is started:
//   system("./server 5001 &");
//   The "&" is required. Without it, system() waits until the server exits and the load balancer freezes.
//
//
// What the server program must do (the load balancer depends on this):
//   - it takes its port as the first argument, for example: ./server 5001
//   - it reads the string until the connection's write side is closed (recv returns 0)
//   - it sends its reply as [status: 1 byte][payload], then closes the connection 


#include <iostream>
#include <string>
#include <cstdlib>
#include <thread>
#include <mutex>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "helper.h"
using namespace std;


// ---- Constants ----
const char *SERVER_PATH = "./server";      // the program we start when the server is down
const int LISTEN_BACKLOG = 16;             // connections the OS holds until accept() takes them
const int MAX_PENDING_CLIENTS = 8;         // clients allowed to wait in our queue. More than this get "server busy".
const int STARTUP_MAX_ATTEMPTS = 50;       // after starting the server, how many times we try to connect
const int STARTUP_POLL_INTERVAL_MS = 100;  // wait between those tries (50 x 100 ms = 5 seconds in total)
int lbPort = 5000;
string serverHost = "127.0.0.0";
int serverPort = 5001;


// ---- Queue: here it holds client sockets (int). ----
// Implement it yourself, do not use std::queue.
template <typename T>
class Queue
{
    struct Node
    {
        T data;
        Node *next;
    };
    Node *front, *rear;
    int count;

public:
    Queue()
    {
        front = nullptr;
        rear = nullptr;
        count = 0;
    }
    ~Queue()
    {
        while (front != nullptr)
        {
            Node *old = front;
            front = front->next;
            delete old;
        }
    }
    Queue(const Queue &) = delete;
    Queue &operator=(const Queue &) = delete;

    void enqueue(const T &val)
    {
        Node *node = new Node{val, nullptr};
        if (rear != nullptr)
            rear->next = node;
        else
            front = node; // the queue was empty
        rear = node;
        count++;
    }
    T dequeue()
    {
        if (front == nullptr)
            return T(); // empty: callers check isEmpty() first
        Node *old = front;
        T val = old->data;
        front = old->next;
        if (front == nullptr)
            rear = nullptr;
        delete old;
        count--;
        return val;
    }
    bool isEmpty()
    {
        return front == nullptr;
    }
    int size()
    {
        return count;
    }
};

struct ClientInfo
{
    int sock;           // the connected socket
    string address;     // the client's IP as text, e.g. "127.0.0.1"
    int port;           // the client's port
    time_t arrivedAt;   // when accept() returned
};

Queue<ClientInfo> pendingClients;
mutex queueMutex;
sig_atomic_t flag = 1;


// ---- Basic socket helpers ----
bool sendError(int sock, const string &message)
{
    // Reply format: [status: 1 byte][payload until close][checksum: 4 bytes].
    // For an error: send one byte with value 1, then the message text. 
    return sendErrorReply(sock, message);
}
int connectToServer(const char *host, int port)
{
    // Create a TCP socket (SOCK_CLOEXEC) and connect it to host:port.
    // Return the socket, or -1 if the connection fails 
    return connectToHost(host, port);
}

// =============================================================================================
// PART 1: WAKING UP THE SERVER IF IT IS DOWN
// =============================================================================================


bool startServer(int port)
{
    // Build the command text: SERVER_PATH + " " + port as text + "&"
    //   Example: "./server 5001 &"
    // Call system() with it.
    // Return true if system() returned 0, otherwise false.
    string cmd = string(SERVER_PATH) + " " + to_string(port) + " &";
    return system(cmd.c_str()) == 0;
}
int connectToServerWithWakeup()
{
    // 1. Try connectToServer(). If it works, return that socket. The server is alive.
    // 2. Otherwise call startServer(). If it fails, return -1.
    // 3. Try connectToServer() up to STARTUP_MAX_ATTEMPTS times 
    // 4. Return the first working socket, or -1 if the server never came up.
    int sock = connectToServer(serverHost.c_str(), serverPort);
    if (sock >= 0){
        return sock; //server is alive
    }
    cerr << "[lb] server not reachable, starting it" << endl;
    if (!startServer(serverPort)){
        return -1;
    }
    for (int attempt = 0; attempt < STARTUP_MAX_ATTEMPTS && flag; attempt++)
    {
        usleep(STARTUP_POLL_INTERVAL_MS * 1000);
        sock = connectToServer(serverHost.c_str(), serverPort);
        if (sock >= 0)
            return sock; //first working socket
    }
    return -1;
}


// =============================================================================================
// PART 2: PASSING THE STRING AND THE REPLY (one client, called by the main thread only)
// =============================================================================================
enum UploadResult
{
    UP_OK,
    UP_CORRUPT,      
    UP_CLIENT_DEAD,  
    UP_BACKEND_DEAD  
};

UploadResult relayUpload(int clientSock, int serverSock) //moves bytes from client to server and sees if it was successful or not
{
    TrailerChecker checker;
    vector<char> in(IO_BUFFER_SIZE), body(IO_BUFFER_SIZE);
    while (true)
    {
        ssize_t n = recv(clientSock, in.data(), in.size(), 0);
        if (n < 0)
        {
            if (errno == EINTR && flag)
                continue;
            return UP_CLIENT_DEAD; 
        }
        if (n == 0)
            break; 
        size_t k = checker.feed(in.data(), (size_t)n, body.data());
        if (k > 0 && !sendAll(serverSock, body.data(), k))
            return UP_BACKEND_DEAD;
    }
    if (!checker.verify())
    {
        shutdown(serverSock, SHUT_WR); // 
        return UP_CORRUPT;
    }
    unsigned char trailer[CHECKSUM_BYTES];
    putU32BE(checker.crcValue(), trailer);
    if (!sendAll(serverSock, (const char *)trailer, CHECKSUM_BYTES))
        return UP_BACKEND_DEAD;
    shutdown(serverSock, SHUT_WR); 
    return UP_OK;
}

bool relayReply(int serverSock, int clientSock) //moves bytes from server to client
{
    TrailerChecker checker;
    vector<char> in(IO_BUFFER_SIZE), body(IO_BUFFER_SIZE);
    uint64_t received = 0;
    bool failed = false;
    while (true)
    {
        ssize_t n = recv(serverSock, in.data(), in.size(), 0);
        if (n < 0)
        {
            if (errno == EINTR && flag)
                continue;
            failed = true;
            break;
        }
        if (n == 0)
            break; 
        received += (uint64_t)n;
        size_t k = checker.feed(in.data(), (size_t)n, body.data());
        if (k > 0 && !sendAll(clientSock, body.data(), k))
            return false; 
    }
    if (received == 0)
    {
        sendError(clientSock, "backend server closed the connection without a reply");
        return false;
    }
    if (failed || !checker.verify())
    {
        cerr << "[lb] reply from the server failed its checksum, trailer withheld" << endl;
        return false;
    }
    unsigned char trailer[CHECKSUM_BYTES];
    putU32BE(checker.crcValue(), trailer);
    return sendAll(clientSock, (const char *)trailer, CHECKSUM_BYTES);
}
void relayOneClient(ClientInfo &c) //uses relayreply and relayupload to communicate with client
{
    // 1. serverSock = connectToServerWithWakeup().
    //    If it is -1: sendError "backend server failed to start", close clientSock, return.
    // 3. Client -> server: repeat recv() from clientSock into a buffer of IO_BUFFER_SIZE and sendAll() it to serverSock.
    //    Stop when recv() returns 0 (the client finished sending) or fails.
    // 4. Call shutdown(serverSock, SHUT_WR), so the server knows the string is complete.
    // 5. Server -> client: repeat recv() from serverSock and sendAll() to clientSock.
    //    Stop when recv() returns 0 (the server sent its whole reply and closed) or fails.
    // 6. Close both sockets.
    setSocketTimeouts(c.sock, CLIENT_TIMEOUT_SEC); // a silent client cannot block everyone behind it forever
    int serverSock = connectToServerWithWakeup();
    if (serverSock < 0)
    {
        cerr << "[lb] cannot reach or start the server" << endl;
        sendError(c.sock, MSG_BACKEND_FAILED);
        gracefulClose(c.sock);
        return;
    }
    setSocketTimeouts(serverSock, CLIENT_TIMEOUT_SEC);
    switch (relayUpload(c.sock, serverSock))
    {
    case UP_OK:
        // 5. Server -> client.
        if (relayReply(serverSock, c.sock))
            cerr << "[lb] served " << c.address << ":" << c.port << endl;
        break;
    case UP_CORRUPT:
        cerr << "[lb] upload from " << c.address << ":" << c.port << " failed its checksum" << endl;
        sendError(c.sock, MSG_UPLOAD_CORRUPT);
        break;
    case UP_CLIENT_DEAD:
        cerr << "[lb] client " << c.address << ":" << c.port << " stopped sending, giving up" << endl;
        break;
    case UP_BACKEND_DEAD:
        sendError(c.sock, "backend server dropped the connection");
        break;
    }
    gracefulClose(serverSock);
    gracefulClose(c.sock);
}



// =============================================================================================
// PART 3: THE TWO THREADS
// =============================================================================================


void acceptLoop(int listenSock)
{
    // Repeat: accept() a new client. 
    //  If pendingClients.size() is already MAX_PENDING_CLIENTS: unlock, sendError "server busy, try again later",
    // else
    // Lock queueMutex.
    //   close the client, and go on.
    //   Otherwise enqueue the client socket.
    // Never wait for the server here, so new clients are always accepted quickly.
    // If accept() fails, print the error and keep going (do not end the thread).
    // Ctrl+C must reach the MAIN thread (the dispatcher), so this thread blocks those signals.
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &mask, nullptr);
    setSocketTimeouts(listenSock, 1);
    while (flag)
    {
        struct sockaddr_in addr;
        socklen_t len = sizeof(addr);
        // SOCK_CLOEXEC again: the server we start with system() must not inherit client sockets.
        int sock = accept4(listenSock, (struct sockaddr *)&addr, &len, SOCK_CLOEXEC);
        if (sock < 0){
            if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK && errno != ECONNABORTED)
            perror("[lb] accept");
                continue;
        }
        char ip[INET_ADDRSTRLEN] = "?";
        inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
        ClientInfo ci{sock, string(ip), (int)ntohs(addr.sin_port), time(nullptr)};

        bool queued = false;
        int waiting = 0;
        {
            lock_guard<mutex> lock(queueMutex); // only one thread may touch the queue at a time
            if (pendingClients.size() < MAX_PENDING_CLIENTS)
            {
                pendingClients.enqueue(ci);
                queued = true;
            }
            waiting = pendingClients.size();
        }
        if (queued)
        {
            cerr << "[lb] client " << ci.address << ":" << ci.port << " queued (" << waiting << " waiting)" << endl;
        }
        else
        {
            // queue full: unlocked already, so the (slow) reply never blocks the dispatcher
            cerr << "[lb] queue full, refusing " << ci.address << ":" << ci.port << endl;
            sendError(sock, MSG_SERVER_BUSY);
            gracefulClose(sock); // acceptLoop owns the socket of a refused client
        }
    }
}
void dispatcherLoop()
{
    // This runs in the main thread and never ends.
    // Repeat:
    //   Lock queueMutex, take ONE client out of the queue if there is one, unlock.
    //   If we got a client: call relayOneClient(). The next client is taken out of the queue only after this one is finished.
    while (flag)
    {
        ClientInfo c;
        bool haveClient = false;
        {
            lock_guard<mutex> lock(queueMutex); // lock, take ONE client if there is one, unlock
            if (!pendingClients.isEmpty())
            {
                c = pendingClients.dequeue();
                haveClient = true;
            }
        }
        if (!haveClient)
        {
            usleep(50 * 1000); // nothing waiting: rest 50 ms instead of burning the CPU
            continue;
        }
        if (time(nullptr) - c.arrivedAt > CLIENT_TIMEOUT_SEC)
        {
            cerr << "[lb] " << c.address << ":" << c.port << " waited too long, dropped (no reply)" << endl;
            close(c.sock); // socket closed, no reply
            continue;
        }
        relayOneClient(c); // the next client is taken only after this one is finished
    }
}

void handler(int sg) {
    (void)sg;
    flag = 0;
}
int main(int argc, char *argv[])
{

    if (argc != 3 && argc != 4)
    {
        cerr << "usage: ./load_balancer <server_ip> <server_port> [listen_port]" << endl;
        return 1;
    }
    serverHost = argv[1];
    serverPort = atoi(argv[2]);
    if (argc == 4)
        lbPort = atoi(argv[3]);
    if (serverPort < 1 || serverPort > 65535 || lbPort < 1 || lbPort > 65535)
    {
        cerr << "invalid port" << endl;
        return 1;
    }

    signal(SIGINT, handler);
    signal(SIGPIPE, SIG_IGN); // a client that vanished must give a send() error, not kill the load balancer

    int listenSock = createListenSocket(lbPort);
    if (listenSock < 0)
    {
        perror("load balancer: cannot listen");
        return 1;
    }
    cerr << "[lb] listening on port " << lbPort << ", server is " << serverHost << ":" << serverPort << endl;

    thread acceptor(acceptLoop, listenSock);
    // 4. Call dispatcherLoop().
    dispatcherLoop();

    // ---- orderly shutdown ----
    flag = 0;
    acceptor.join();
    close(listenSock);
    {
        lock_guard<mutex> lock(queueMutex);
        while (!pendingClients.isEmpty())
            close(pendingClients.dequeue().sock); // clients still waiting are simply closed
    }
    cerr << "[lb] stopped" << endl;
    return 0;
}