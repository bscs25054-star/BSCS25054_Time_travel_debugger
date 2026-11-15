
// The client never talks to the server directly. It only talks to the load balancer.
// The load balancer wakes the server up if it is down, so the client does not care.
// The first "start" may just take a bit longer.
//
// The network is used ONLY by "start". After the .tdbg file is downloaded,
// next / prv / jmp read that file on the local disk. They never contact the load balancer.
//
// How the pieces fit together:
//   main loop -> readCommand -> runCommand
//   start -> cmdStart()  -> requestDebugFile() (Part A) -> openTdbg() (Part B) -> shows step 1
//   next, prv, jmp -> moveBy() (Part D) -> readSnapshotAt() (Part B) -> printSnapshot() (Part E)

#include <iostream>
#include <string>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include <signal.h>
#include <vector>
#include <sys/socket.h>
#include "helper.h"
using namespace std;

// ---- Constants ----
const char *PROGRAM_NAME = "ttdb";
const char *PROMPT = "ttdb> ";
const char *TDBG_FILE = "ttdb_last.tdbg"; // where the downloaded .tdbg is saved
const int MAX_JUMP = 1000000000;          // biggest n accepted by jmp
const int MAX_NAME_BYTES = 4096;
string LB_HOST = "127.0.0.0";
int LB_PORT = 5000;

// ---- Structs only the client needs ----
struct TdbgReader
{
    ssize_t file_index;
    TTDBHeader header;
    vector<int64_t> index;
};
struct DebugSession
{
    int active_flag;     // false until a "start" has worked
    TdbgReader reader;   // the .tdbg file stays open for the whole session
    int64_t currentStep; 
};

// =============================================================================================
// PART A: TALKING TO THE LOAD BALANCER
// =============================================================================================

bool uploadSource(int sock, const char *sourcePath)
{
    // have to do checksum and then send
    // When the file is finished, call shutdown(sock, SHUT_WR). This is how the server knows the string is complete.
    // Return false if the file cannot be opened or any send fails
    FILE *f = fopen(sourcePath, "rb");
    if (!f)
        return false;
    CRC32 crc;
    char status = (char)STATUS_OK;
    crc.update(&status, 1); 
    bool ok = sendAll(sock, &status, 1);
    vector<char> buf(IO_BUFFER_SIZE);
    while (ok)
    {
        size_t n = fread(buf.data(), 1, buf.size(), f);
        if (n == 0)
            break;
        crc.update(buf.data(), n);
        ok = sendAll(sock, buf.data(), n);
    }
    bool readFailed = ferror(f) != 0;
    fclose(f);
    if (ok && !readFailed)
    {
        unsigned char trailer[CHECKSUM_BYTES];
        putU32BE(crc.value(), trailer);
        ok = sendAll(sock, (const char *)trailer, CHECKSUM_BYTES);
    }
    shutdown(sock, SHUT_WR);
    return ok && !readFailed;
}

bool receiveResponse(int sock, const char *outTdbgPath, string &outError)
{
    // Reply on the wire: [status][payload][CRC-32]. The CRC covers the status byte too.
    // Feed every received byte (status included) to a TrailerChecker.
    // Status 1: collect the payload into outError. Status 0: write the payload to the file.
    // At the end call verify(). If it fails, delete the file, set outError to
    // "checksum mismatch, response corrupted" and return false.
    TrailerChecker checker;
    vector<char> in(IO_BUFFER_SIZE), body(IO_BUFFER_SIZE);
    FILE *out = nullptr;
    bool needStatus = true;
    uint8_t status = 0;
    string errorText;
    uint64_t received = 0;
    bool connectionFailed = false;

    while (true)
    {
        ssize_t n = recv(sock, in.data(), in.size(), 0);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            connectionFailed = true; 
            break;
        }
        if (n == 0)
            break; 
        received += (uint64_t)n;
        size_t k = checker.feed(in.data(), (size_t)n, body.data());
        size_t off = 0;
        if (k > 0 && needStatus)
        {
            status = (uint8_t)body[0];
            needStatus = false;
            off = 1;
            if (status == STATUS_OK)
            {
                out = fopen(outTdbgPath, "wb");
                if (!out)
                {
                    outError = string("cannot create ") + outTdbgPath;
                    return false;
                }
            }
        }
        if (k > off)
        {
            if (status == STATUS_OK && out)
                fwrite(body.data() + off, 1, k - off, out);
            else if (status == STATUS_ERROR && errorText.size() < 65536)
                errorText.append(body.data() + off, k - off);
        }
    }
    if (out)
        fclose(out);

    if (received == 0)
    {
        outError = "no reply (the load balancer closed the connection; maybe you waited too long in the queue)";
        remove(outTdbgPath);
        return false;
    }
    if (connectionFailed)
    {
        outError = "connection lost or timed out while receiving the reply";
        remove(outTdbgPath);
        return false;
    }
    if (!checker.verify())
    {
        remove(outTdbgPath);
        outError = MSG_RESPONSE_CORRUPT;
        return false;
    }
    if (status == STATUS_ERROR)
    {
        remove(outTdbgPath);
        outError = errorText;
        return false;
    }
    if (status != STATUS_OK)
    {
        remove(outTdbgPath);
        outError = MSG_RESPONSE_CORRUPT;
        return false;
    }
    return true;
}
//its purpose is to bundle the whole network exchange into one call, it uses upload source, recieve source and connecttohost
//to establish an entire connection with the loadbalancer and close the socket gracefully
bool requestDebugFile(const string &host, int port, const string &sourcePath, const string &outTdbgPath, string &outError)
{
    FILE *probe = fopen(sourcePath.c_str(), "rb"); 
    if (!probe)
    {
        outError = "cannot open source file '" + sourcePath + "'";
        return false;
    }
    fclose(probe);

    int sock = connectToHost(host.c_str(), port);
    if (sock < 0)
    {
        outError = "cannot connect to the load balancer at " + host + ":" + to_string(port);
        return false;
    }
    setSocketTimeouts(sock, CLIENT_TIMEOUT_SEC);

    bool uploaded = uploadSource(sock, sourcePath.c_str());
    string err;
    bool ok = receiveResponse(sock, outTdbgPath.c_str(), err); 
    close(sock);
    if (ok)
        return true;
    outError = err;
    if (!uploaded)
        outError += " (the upload did not complete)";
    return false;
}

// =============================================================================================
// PART B: READING THE .tdbg FILE
// File layout (written by Phase 1):
//   header | snapshot 0 | snapshot 1 | ... | index (one 8 byte offset per snapshot)
// The index lets us jump straight to any step without reading the steps before it.
// ===============================s==============================================================
static bool readExact(int fd, void *buf, size_t n)
{
    char *p = static_cast<char *>(buf);
    while (n > 0)
    {
        ssize_t k = read(fd, p, n);
        if (k < 0)
        {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (k == 0)
            return false; // end of file too early
        p += k;
        n -= (size_t)k;
    }
    return true;
}
static bool readI32(int fd, int32_t &v)
{
    return readExact(fd, &v, sizeof(int32_t));
}
static bool readStr(int fd, string &s) // [length 4B][characters]
{
   int32_t len;
    if (!readI32(fd, len) || len < 0 || len > MAX_NAME_BYTES)
        return false;
    s.resize((size_t)len);
    return len == 0 || readExact(fd, &s[0], (size_t)len);
}
static bool readVars(int fd, Variable vars[], int32_t count)
{
    for (int32_t i = 0; i < count; i++)
        if (!readStr(fd, vars[i].name) || !readI32(fd, vars[i].value))
            return false;
    return true;
}
static bool readSnapshotBody(int fd, Snapshot &s)
{
    if (!readI32(fd, s.stackDepth) || s.stackDepth < 0 || s.stackDepth > MAX_STACK_DEPTH)
        return false;
    for (int32_t i = 0; i < s.stackDepth; i++)
    {
        Frame &fr = s.callStack[i];
        if (!readStr(fd, fr.func_name) || !readI32(fd, fr.argc) || fr.argc < 0 || fr.argc > MAX_VARS_PER_FRAME)
            return false;
        if (!readVars(fd, fr.argv, fr.argc) || !readI32(fd, fr.returnLine))
            return false;
        if (!readI32(fd, fr.localCount) || fr.localCount < 0 || fr.localCount > MAX_VARS_PER_FRAME)
            return false;
        if (!readVars(fd, fr.locals, fr.localCount))
            return false;
    }
    return true;
}
void closeTdbg(TdbgReader &r)
{
    // Free r.index and close the file.
    if (r.file_index >= 0)
        close((int)r.file_index);
    r.file_index = -1;
    vector<int64_t>().swap(r.index); // really releases the memory (clear() would keep the capacity)
}
Snapshot *readSnapshotAt(TdbgReader &r, int32_t step)
{
    if (r.file_index < 0 || step < 0 || step >= r.header.stepCount)
        return nullptr;
    if (lseek((int)r.file_index, (off_t)r.index[(size_t)step], SEEK_SET) < 0) // the "time travel" jump
        return nullptr;
    Snapshot *s = new Snapshot(); // heap: ~85 KB
    if (!readSnapshotBody((int)r.file_index, *s))
    {
        delete s;
        return nullptr;
    }
    return s;
}
bool openTdbg(const char *path, TdbgReader &r)
{
    r.file_index = -1;
    vector<int64_t>().swap(r.index);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    TTDBHeader h;
    if (!readExact(fd, h.magic, 4) || !readI32(fd, h.version) || !readI32(fd, h.stepCount) ||
        !readExact(fd, &h.indexOffset, sizeof(int64_t)))
    {
        close(fd);
        return false;
    }
    if (memcmp(h.magic, "TTDB", 4) != 0 || h.version != TDBG_VERSION || h.stepCount < 1)
    {
        close(fd);
        return false;
    }
    off_t fileSize = lseek(fd, 0, SEEK_END);
    if (fileSize < 0 || h.indexOffset < TDBG_HEADER_BYTES ||
        h.indexOffset + (int64_t)sizeof(int64_t) * h.stepCount != (int64_t)fileSize)
    {
        close(fd);
        return false;
    }
    vector<int64_t> index((size_t)h.stepCount);
    if (lseek(fd, (off_t)h.indexOffset, SEEK_SET) < 0 ||
        !readExact(fd, index.data(), sizeof(int64_t) * (size_t)h.stepCount))
    {
        close(fd);
        return false;
    }
    for (int32_t i = 0; i < h.stepCount; i++)
    {
        bool inside = index[(size_t)i] >= TDBG_HEADER_BYTES && index[(size_t)i] < h.indexOffset;
        bool ordered = (i == 0) ? index[0] == TDBG_HEADER_BYTES : index[(size_t)i] > index[(size_t)i - 1];
        if (!inside || !ordered)
        {
            close(fd);
            return false;
        }
    }
    r.file_index = fd;
    r.header = h;
    r.index = index;
    Snapshot *first = readSnapshotAt(r, 0);
    Snapshot *last = readSnapshotAt(r, h.stepCount - 1);
    bool good = first != nullptr && last != nullptr && lseek(fd, 0, SEEK_CUR) == (off_t)h.indexOffset;
    delete first;
    delete last;
    if (!good)
    {
        closeTdbg(r);
        return false;
    }
    return true;
}


// =============================================================================================
// PART C: THE SESSION IN MEMORY
// There is one DebugSession variable, created in main() and passed to every command by reference.
// =============================================================================================

void initSession(DebugSession &s)
{
    s.active_flag = 0;
    s.reader.file_index = -1;
    memset(&s.reader.header, 0, sizeof(s.reader.header));
    s.currentStep = 0;
}
void closeSession(DebugSession &s)
{
    if (s.active_flag)
        closeTdbg(s.reader);
    s.active_flag = 0;
    // If active, call closeTdbg(s.reader). Then set active = false.
    // Called when the user types quit and when the input ends.
}

// =============================================================================================
// PART D: THE COMMANDS
// =============================================================================================

void printSnapshot(int32_t step, int32_t totalSteps, const Snapshot &s); // defined in Part E

bool showCurrent(DebugSession &s)
{
    // Read the snapshot at s.currentStep, call printSnapshot, delete the snapshot.
    // If readSnapshotAt returns nullptr, print "damaged .tdbg file" and return false.
     Snapshot *snap = readSnapshotAt(s.reader, (int32_t)s.currentStep);
    if (snap == nullptr) { cout << "damaged .tdbg file" << endl; return false; }
    printSnapshot((int32_t)s.currentStep + 1, s.reader.header.stepCount, *snap);
    delete snap;
    return true;
}
static bool needSession(const DebugSession &s)
{
    if (!s.active_flag) { 
        cout << "No session yet. Use: start <source file>" << endl;
         return false; 
    }
    return true;
}
bool moveBy(DebugSession &s, int32_t delta)
{
    if (!needSession(s))
        return false;
    int64_t total = s.reader.header.stepCount;
    int64_t target = s.currentStep + delta;
    if (target < 0)
    {
        if (delta == -1)
            cout << "Already at the first step." << endl;
        else
            cout << "Cannot go back " << -(int64_t)delta << " steps: only " << s.currentStep << " step(s) come before this one." << endl;
        return false;
    }
    if (target >= total)
    {
        if (delta == 1)
            cout << "Already at the last step." << endl;
        else
            cout << "Cannot go forward " << delta << " steps: only " << (total - 1 - s.currentStep) << " step(s) come after this one." << endl;
        return false;
    }
    s.currentStep = target;
    return showCurrent(s);
}
bool cmdStart(DebugSession &s, const string &sourcePath)
{
    // 1. requestDebugFile with host and port, saving to TDBG_FILE.
    // 2. If it fails, print the error text and return false. Keep the old session as it was.
    // 3. Open the new file with openTdbg into a NEW local TdbgReader.
    //    If that fails, print "server sent a damaged .tdbg file", keep the old session, return false.
    // 4. Only now close the old reader (if s.active), then put the new reader into s,
    //    set s.currentStep = 0 and s.active = true.
    // 5. Show step 1 with showCurrent(s).
     string partFile = string(TDBG_FILE) + ".part";
    string err;
    cout << "Sending " << sourcePath << " to " << LB_HOST << ":" << LB_PORT << " ..." << endl;
    if (!requestDebugFile(LB_HOST, LB_PORT, sourcePath, partFile, err))
    {
        cout << "Error: " << err << endl;
        remove(partFile.c_str());
        return false;
    }
    TdbgReader fresh;
    fresh.file_index = -1;
    if (!openTdbg(partFile.c_str(), fresh))
    {
        cout << "Error: " << MSG_BAD_TDBG << (s.active_flag ? " (your previous session is kept)" : "") << endl;
        remove(partFile.c_str());
        return false;
    }
    if (s.active_flag)
        closeTdbg(s.reader);
    rename(partFile.c_str(), TDBG_FILE); 
    s.reader = fresh;
    s.currentStep = 0;
    s.active_flag = 1;
    cout << "Session started: " << s.reader.header.stepCount << " step(s). Saved as " << TDBG_FILE << endl;
    return showCurrent(s);
}
bool cmdNext(DebugSession &s) { return moveBy(s, 1);}
bool cmdPrv(DebugSession &s) { return moveBy(s, -1);}
bool cmdJmpNext(DebugSession &s, int32_t n) { return moveBy(s, n);}
bool cmdJmpPrv(DebugSession &s, int32_t n) { return moveBy(s, -n);}
bool cmdJmpTo(DebugSession &s, int32_t step)
{
    if (!needSession(s)) return false;
    if (step < 1 || step > s.reader.header.stepCount)
    { cout << "Step must be between 1 and " << s.reader.header.stepCount << "." << endl; return false; }
    s.currentStep = step - 1;
    return showCurrent(s);
}

// =============================================================================================
// PART E: PRINTING
// =============================================================================================

static string varList(const Variable vars[], int32_t count)
{
    if (count == 0)
        return "-";
    string out;
    for (int32_t i = 0; i < count; i++)
        out += (i ? ", " : "") + vars[i].name + "=" + to_string(vars[i].value);
    return out;
}
void printSnapshot(int32_t step, int32_t totalSteps, const Snapshot &s)
{ 
      cout << "--- Step " << step << " of " << totalSteps << " (call stack depth " << s.stackDepth << ") ---" << endl;
    if (s.stackDepth == 0)
        cout << "  (program finished: the call stack is empty)" << endl;
    for (int32_t i = 0; i < s.stackDepth; i++)
    {
        const Frame &fr = s.callStack[i];
        cout << (i == 0 ? "  -> " : "     ") << fr.func_name << "(" << varList(fr.argv, fr.argc) << ")"
             << "  locals: " << varList(fr.locals, fr.localCount) << endl;
    }
}
void printHelp()
{
    cout << "Commands:" << endl
         << "  start <file>      send a C-- source file to the server and open the result at step 1" << endl
         << "  next              go one step forward" << endl
         << "  prv               go one step back" << endl
         << "  jmp next <n>      go n steps forward" << endl
         << "  jmp prv <n>       go n steps back" << endl
         << "  jmp <n>           go straight to step n (counting from 1)" << endl
         << "  help              show this text" << endl
         << "  quit | exit       leave" << endl;
    // Print the commands
}

// =============================================================================================
// PART F: THE INFINITE LOOP
// =============================================================================================

static bool parsePositive(const string &w, int32_t &out) // a whole number from 1 to MAX_JUMP
{
    if (w.empty())
        return false;
    char *end = nullptr;
    errno = 0;
    long v = strtol(w.c_str(), &end, 10);
    if (*end != '\0' || errno == ERANGE || v < 1 || v > MAX_JUMP)
        return false;
    out = (int32_t)v;
    return true;
}
static bool badCommand()
{
    cout << "unknown command" << endl;
    printHelp();
    return false;
}
bool runCommand(DebugSession &s, const string &line, bool &quitRequested)
{
    // 1. Split the line into words.
    // 2. If the first word is "ttdb", drop it.
    // 3. Match the first word:
    //      start      -> needs a path after it, then cmdStart
    //      next       -> no more words, then cmdNext
    //      prv        -> no more words, then cmdPrv
    //      jmp        -> second word "next" or "prv", third word a positive number ,
    //                    then cmdJmpNext or cmdJmpPrv
    //      help       -> printHelp
    //      quit, exit -> set quitRequested = true
    // 4. For anything else, or a wrong number of words, print "unknown command" and the help text, and return false.
    vector<string> w;
    istringstream in(line);
    string word;
    while (in >> word)
        w.push_back(word);
    if (!w.empty() && w[0] == PROGRAM_NAME)
        w.erase(w.begin());
    if (w.empty())
        return badCommand();
    const string &cmd = w[0];
    int32_t n = 0;
    if (cmd == "start")
    {
        if (w.size() != 2)
            return badCommand();
        return cmdStart(s, w[1]);
    }
    if (cmd == "next")
        return w.size() == 1 ? cmdNext(s) : badCommand();
    if (cmd == "prv")
        return w.size() == 1 ? cmdPrv(s) : badCommand();
    if (cmd == "jmp")
    {
        if (w.size() == 3 && w[1] == "next" && parsePositive(w[2], n))
            return cmdJmpNext(s, n);
        if (w.size() == 3 && w[1] == "prv" && parsePositive(w[2], n))
            return cmdJmpPrv(s, n);
        if (w.size() == 2 && parsePositive(w[1], n))
            return cmdJmpTo(s, n);
        return badCommand();
    }
    if (cmd == "help")
    {
        if (w.size() != 1)
            return badCommand();
        printHelp();
        return true;
    }
    if (cmd == "quit" || cmd == "exit")
    {
        quitRequested = true;
        return true;
    }
    return badCommand();
}

int main(int argc, char *argv[])
{
    if (argc != 3)
    {
        cerr << "usage: ./client <lb_ip> <lb_port>" << endl;
        return 1;
    }
    LB_HOST = argv[1];
    LB_PORT = atoi(argv[2]);
    if (LB_PORT < 1 || LB_PORT > 65535)
    {
        cerr << "invalid port: " << argv[2] << endl;
        return 1;
    }
    signal(SIGPIPE, SIG_IGN); // a closed connection must give an error, not kill the client

    DebugSession session;
    initSession(session);
    cout << "Time-Travel Debugger client. Load balancer: " << LB_HOST << ":" << LB_PORT << endl;
    printHelp();

    bool quitRequested = false;
    string line;
    while (true)
    {
        if (quitRequested)
            break;
        cout << PROMPT << flush;
        if (!getline(cin, line)) // end of input (Ctrl+D) leaves the loop too
            break;
        runCommand(session, line, quitRequested); // an error only prints a message, it never ends the loop
    }
    closeSession(session);
    return 0;
    // you get string LB_HOST and int LB_PORT = 0 in arguments of this main function. 

    // DebugSession session; initSession(session);
    // Print a short welcome and the help text.
    //
    // Infinite loop:
    //   if quitRequested, break
    //
    // After the loop: closeSession(session) and return 0.
    // Never exit the loop because of an error. Only quit or the end of the input leaves it.
}