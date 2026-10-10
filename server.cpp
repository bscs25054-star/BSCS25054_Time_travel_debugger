// ======================= TIME-TRAVEL DEBUGGER - SERVER TEMPLATE =======================

// Pipeline this file implements, top to bottom:
//   0. Receive  -- stream the client's .trace bytes straight to source.bin on disk
//   1. Pass 0X0   -- validity check (FUNC/FUNC_END matching)
//   2. Pass 0X1   -- resolve(): copy EVERY source line into resolve.bin as [offset][size][string], then patch CALL targets.
//   3. Pass 0X2   -- execute resolve.bin: tokenize ONE line at a time, update the call stack, take a snapshot -> Timeline
//   4. Pass 0X3   -- serialize Timeline -> session.tdbg(header + snapshot records + dense index)


#include <iostream>
#include <string>
#include <cstdint>
#include <fstream>
#include <unistd.h>
#include <sys/socket.h>
#include <cstdint>
#include <cctype>
#include <cstdio>
using namespace std;

// ---- Constants ----
const int32_t MAX_VARS_PER_FRAME = 16;
const int32_t MAX_STACK_DEPTH = 64;
const int32_t MAX_FUNCS = 128;
const int32_t MAX_TOKENS = MAX_VARS_PER_FRAME + 2; // kW + func_name + upto 16 params/args
const int32_t MAX_PATCHES = MAX_FUNCS * 4;
const uint64_t MAX_SOURCE_BYTES = 15ULL * 1024 * 1024; // sanity cap on the declared file length
const int32_t IO_BUFFER_SIZE = 64 * 1024;                  // fixed buffer for streaming to/from disk
const int32_t SOCKET_TIMEOUT_SEC = 5;                      // TODO: apply as SO_RCVTIMEO so a deadclient can't hang the server forever

// ---- Custom data structures

// Stack: back the live Call Stack during execution
template <typename T>
class Stack
{
    struct Node
    {
        T data;
        Node *next;
    };
    Node *top;
    int32_t count;

public:
    // Implement these functions:
    Stack()
    { top = nullptr;
      count = 0;
    }
    ~Stack() {
        while (top != nullptr){
            Node *old = top;
            top = top->next;
            delete old;
        }
    }
    void push(const T &val)
    {
      if(count >= MAX_STACK_DEPTH){
        return;
      }
      Node* newval = new Node{val,top};
      top = newval;
      count++;
        // pushes the value on the stack if max limit is not reached yet.
    }
    T pop()
    {
        if(isEmpty()){
            return T();
        }
        Node* oldptr = top;
        T val = oldptr->data;
        top = oldptr->next;
        delete oldptr;
        count--;
        return val;
        // pop the top value on the stack
    }
    T &peek()
    {
        if(isEmpty()){
             static T empty = T();
             return empty;
        }
        return top->data;
        // returns the top value on the stack
    }
    bool isEmpty()
    {
        return top == nullptr;
    }
    int32_t depth()
    {
        return count;
    }
    int32_t snapshot_into(T out[], int32_t maxLen)
    {
        int32_t written = 0;
        for(Node* currentptr = top; currentptr != nullptr && written < maxLen; currentptr = currentptr->next){
            out[written] = currentptr->data;
            written++;
        }
        return written;
        // copies every frame, top to bottom in the array given as a parameter
        // this is what buildSnapshot() call, returns count written
    }
};


// Timeline : doubly linked list of Snapshots
struct Snapshot; // fwd declaration;
struct TimelineNode
{
    Snapshot *data;
    TimelineNode *next;
    TimelineNode *prev;
};
class Timeline
{
    TimelineNode *head, *tail;
    int32_t stepCount;

public:
    // Implement these functions
    Timeline()
    {
    }
    void record(Snapshot *s)
    {
        // add record in the timeline
    }
    TimelineNode *begin()
    {
    }
    int32_t getStepCount()
    {
    }
};

// Core structs
struct Variable
{
    string name;
    int32_t value;
};
struct Frame
{
    string func_name;
    int32_t argc;
    Variable argv[MAX_VARS_PER_FRAME];
    int32_t returnLine;
    Variable locals[MAX_VARS_PER_FRAME];
    int32_t localCount;
};
struct Snapshot
{
    Frame callStack[MAX_STACK_DEPTH];
    int32_t stackDepth;
};
struct TTDBHeader
{
    char magic[4]; // "TTDB"
    int32_t version;
    int32_t stepCount;
    int64_t indexOffset;
};
void writeHeader(FILE *f, const TTDBHeader &h)
{
    fwrite(h.magic, 1, 4, f);
    fwrite(&h.version, sizeof(int32_t), 1, f);

    // placeholder for other two data members
}

// resolve.bin - bookkeeping
struct FuncEntry
{
    string funcName;
    int64_t byteOffsetInResolveBin; // where this function's FUNC header record sits
};
struct PendingPatch
{
    int64_t byteOffsetOfOffsetField; // where in resolve.bin to seek back and overwrite
    string targetFuncName;
};

static bool spaceseperatedword(const string &s, size_t&pos, string &out){
    size_t ind = s.size();
    while (pos < ind && isspace((unsigned char)s[pos]))
        pos++;
    if (pos >= ind )
        return false;
    size_t start = pos;
    while (pos < ind && !isspace((unsigned char)s[pos]))
        pos++;
    out = s.substr(start, pos - start);
    return true;
}
// PASS 0x0: READING source.bin + VALIDITY CHECK
bool readSourceLine(ifstream &in, string &out)
{
    string currentline;
    while(getline(in, currentline)){
        if(!currentline.empty() && currentline.back() == '\r'){
            currentline.pop_back();
        }
        if(currentline.find_first_not_of("\t") == string::npos)  {
            continue;
        } 
        out = currentline;
        return true;
    }
    return false;
    // reads the next nonblank line
}
string firstWord(const string &line)
{
    size_t pos = 0;
    string w;
    if (spaceseperatedword(line, pos, w)){
        return w;
    }
    return "";
    // returns first word from the input string
}
string secondWord(const string &line)
{   
     size_t pos = 0;
    string w;
    if (!spaceseperatedword(line, pos, w)){
        return "";
    }
    if (spaceseperatedword(line, pos, w)){
        return w;
    }
    return "";
    // returns the second word
}
bool validateProgram(const char *sourcePath)
{
    ifstream in(sourcePath);
    if (!in) {
        cerr << "Validation error: cannot open " << sourcePath << endl;
        return false;
    }
    Stack<int32_t> openfunctions;
    string currentline;
    int32_t instrNo = 0;
    while (readSourceLine(in, currentline)){
        instrNo++;
        string kw = firstWord(currentline);
        if (kw == "func") {
            if (secondWord(currentline).empty()){
                cerr << "Validation error: instruction " << instrNo << ": 'func' needs a function name" << endl;
                return false;
            }
            if (!openfunctions.isEmpty()){
                cerr << "Validation error: instruction " << instrNo << ": nested func (the func from instruction "
                     << openfunctions.peek() << " is still open)" << endl;
                return false;
            }
            openfunctions.push(instrNo);
        }
        else if (kw == "func_end") {
            if (openfunctions.isEmpty())
            {
                cerr << "Validation error: instruction " << instrNo << ": func_end without a matching func" << endl;
                return false;
            }
            openfunctions.pop();
        }
    }
    if (!openfunctions.isEmpty()) {
        cerr << "Validation error: the func at instruction " << openfunctions.peek() << " is never closed with func_end" << endl;
        return false;
    }
    return true;
    // for each func defined there should be exactly one func_end and no nested funcs allowed - 
}

// PASS 0x1: RESOLVE() -> resolve.bin
int64_t writeResolveRecord(FILE *f, int64_t offsetField, const string &text)
{
    int64_t startcurs = ftell(f);
    int32_t txtsize = (int32_t)text.size();
    if((fwrite(&offsetField, sizeof(int64_t), 1, f) != 1)){
        return -1;
    }
    if((fwrite(&txtsize, sizeof(int32_t), 1, f) != 1)){
        return -1;
    }
    if(( txtsize < 0 || fwrite(text.data(), 1, size_t(txtsize), f) != size_t(txtsize))){
        return -1;
    }
    return startcurs;
    // writes one [offset(8B)][size(4B)][string] record at the current file position
    // returns this record's own starting byte position
}
int64_t readResolveRecord(FILE *f, string &outText)
{
    int64_t startoffset;
    int32_t size;
    if((fread(&startoffset, sizeof(int64_t), 1, f) != 1)){ // has offset been read properly
        return -1;
    }
    if((fread(&size, sizeof(int32_t), 1, f) != 1)){ // has size been read properly
        return -1;
    }
    if( size < 0 || size > MAX_SOURCE_BYTES){  //making sure string is not bigger than a certain length
        return -1;
    }
    outText.resize(size);
    if(( size < 0 || fread(&outText[0], 1, size_t(size), f) != size_t(size))){ //making sure text has been read
        return -1;
    }
    return startoffset;
    // reads one record at the current position and advances past it, returns the offset field - the raw line text comes back untouched in outText.
}
static int32_t findFunc(FuncEntry funcs[], int32_t count, const string &name)
{
    for (int32_t i = 0; i < count; i++)
        if (funcs[i].funcName == name)
            return i;
    return -1;
}


int64_t resolveProgram(const char *sourcePath, const char *resolveBinPath)
{
    FuncEntry funcArray[MAX_FUNCS];
    int32_t funcCount = 0;
    PendingPatch patches[MAX_PATCHES];
    int32_t patchCount = 0;
    cerr << "C: resolveProgram started" << endl;
    ifstream filereader(sourcePath);
    if(!filereader){
        cerr << "err, could not open file"<< endl;
        return -1;
    }

    FILE* Resolve = fopen(resolveBinPath, "w+b");
      cerr << "D: resolve.bin opened" << endl;
    if(!Resolve){
        cerr << "error, could not open file" << endl;
        return -1;
    }

    string currentline;
    while(readSourceLine(filereader, currentline)){
        string firstword = firstWord(currentline);
        int64_t here = ftell(Resolve);
        int64_t recordpos = writeResolveRecord(Resolve,here, currentline);
        if (recordpos < 0){
            cerr << "error: failed writing " << resolveBinPath << endl;
            fclose(Resolve);
            return -1;
        }
        if(firstword == "func"){
            string second = secondWord(currentline);
            if(findFunc(funcArray,funcCount,second) >= 0){
                cerr <<  "error function " << second << " defined more than once" << endl;
                fclose(Resolve);
                return -1;
            }
            if(funcCount >= MAX_FUNCS){
                cerr <<  "too many functions defined, limit reached" << endl;
                fclose(Resolve);
                return -1;
            }
            funcArray[funcCount].funcName = second;
            funcArray[funcCount].byteOffsetInResolveBin = recordpos;
            funcCount++;
        }
        if(firstword == "call") {
            string targfunc = secondWord(currentline);
            if(targfunc.empty()){
                cerr <<  "call needs a function name, cannot be empty" << endl;
                fclose(Resolve);
                return -1;
            }
            if(patchCount >= MAX_PATCHES){
                cout <<  "too many functions call instructions" << endl;
                fclose(Resolve);
                return -1;
            }
            patches[patchCount].byteOffsetOfOffsetField = recordpos;
            patches[patchCount].targetFuncName = targfunc;
            patchCount++;
        }
    }
    int32_t mainpos =findFunc(funcArray,funcCount, "main");
    if(mainpos < 0){
        cerr << "error, program has no main function" << endl;
        fclose(Resolve);
        return -1;
    }
    for (int32_t i = 0; i < patchCount; i++)
    {
        int32_t idx = findFunc(funcArray, funcCount, patches[i].targetFuncName);
        if (idx < 0)
        {
            cerr << "Resolve error: call to undefined function '" << patches[i].targetFuncName << "'" << endl;
            fclose(Resolve);
            return -1;
        }
        int64_t target = funcArray[idx].byteOffsetInResolveBin;
        fseek(Resolve, (long)patches[i].byteOffsetOfOffsetField, SEEK_SET);
        fwrite(&target, sizeof(int64_t), 1, Resolve);
    }
    fclose(Resolve);
     cerr << "E: resolve finished" << endl;
    return funcArray[mainpos].byteOffsetInResolveBin;
    // Every source line becomes one record holding the raw line, as-is.
    // resolve() only PEEKS at the leading word(s) -- enough to spot FUNC
    // (remember its position) and CALL (remember which function it needs
    // and where its offset field sits).
    // Once the whole file is written, every CALL's offset field is patched
    // with its target's position. Patching happens after the full write
    // Returns the byte offset of main's FUNC header record.
    // if there is no main return the error 
}

// PASS 0x2: EXECUTION (tokenization happens here)
enum TokenType
{
    KEYWORD,
    IDENTIFIER,
    PARAM
};
struct Token
{
    TokenType type;
    string text;
};
int32_t tokenizeLine(const string &line, Token tokens[], int32_t maxTokens)
{
    // first word is always a instruction keyword
    // instruction set = [func, func_end, call, set, add, sub, mul and div]
    // next word is identifier like name of a function, variable name
    // after identifier all are the params/arg, space separated
}
Snapshot *buildSnapshot(Stack<Frame> &callStack)
{
    // build the snapshot based on the callStack given
}
void executeProgram(const char *resolveBinPath, int64_t mainOffset, Timeline &timeline)
{
    // initialize the call stack
    // make the main frame
    // push main frame on the call stack

    // implementation:
    // execute line by line, and according to the keyword perform action
}

// PASS 0x3: SERIALIZE TIMELINE
void writeTdbg(Timeline &timeline, const char *tdbgPath)
{
    // placeholder for header
    // index array of the size of stepcount from the timeline
    // placing each snapshot in the file while maintaining the index(starting point of each nth snapshot)
    // after timeline add the index array i the file
    // update the header
}
// main section

// /home/nashwa_khan/server.cpp
int32_t main(){
    cerr << "TEST 123" << endl;
    cerr << "A: main started" << endl;
    if (!validateProgram("source.bin"))
    {
        return 1;
    }
     cerr << "B: validation passed" << endl;
    int64_t mainOffset = resolveProgram("source.bin", "resolve.bin");
    if (mainOffset < 0)
    {
        return 1;
    }
    cout << "main is at byte " << mainOffset << endl;

    FILE *f = fopen("resolve.bin", "rb");
    string text;
    int64_t start = ftell(f);
    int64_t off;
    while ((off = readResolveRecord(f, text)) >= 0)
    {
        cout << "start " << start << " | offset field " << off << " | " << text << endl;
        start = ftell(f);
    }
    fclose(f);
    return 0;
}