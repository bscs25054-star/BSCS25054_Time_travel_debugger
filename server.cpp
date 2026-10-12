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
#include <cstdlib>   
#include <cerrno>    
using namespace std;

// ---- Constants ----
const int32_t MAX_STEPS = 10000;  
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
        head = tail = nullptr;
        stepCount = 0;

    }
    void record(Snapshot *s)
    {
        TimelineNode* newrecord = new TimelineNode{s, nullptr, tail};
        if( tail == nullptr){
            head = tail = newrecord;
            stepCount++;
            return;
        }
        tail->next = newrecord;
        tail = tail->next;
    }
    TimelineNode *begin()
    {
        return head;
    }
    int32_t getStepCount()
    {
        return stepCount;
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
    int32_t wordcount = 0;
    size_t cursorpos = 0;
    string word;
    while(spaceseperatedword(line,cursorpos,word)){
        if(wordcount >= maxTokens){ //Too Many Parameters(error)
            return -1;
        }
        tokens[wordcount].text = word;
        if(wordcount == 0){
            tokens[wordcount].type = IDENTIFIER;
        }
        else if(wordcount == 1){
             tokens[wordcount].type = KEYWORD;
        }
        else{
             tokens[wordcount].type = PARAM;
        }
        wordcount++;
    }
    return wordcount;
    // first word is always a instruction keyword
    // instruction set = [func, func_end, call, set, add, sub, mul and div]
    // next word is identifier like name of a function, variable name
    // after identifier all are the params/arg, space separated
}
Snapshot *buildSnapshot(Stack<Frame> &callStack)
{
    Snapshot* currscreenshot = new Snapshot();
    currscreenshot->stackDepth = callStack.snapshot_into(currscreenshot->callStack, MAX_STACK_DEPTH);
    return currscreenshot;
    // build the snapshot based on the callStack given
}
static bool IntegerParse(const string &s, int32_t& out){ //Determines if this word is a whole number
    if(s.empty()){
        return false;
    }
    char* end = nullptr;
    errno = 0;
    long long v = strtoll(s.c_str(), &end, 10); 
    if(*end != '\0' || errno == ERANGE){
        return false;
    }
    if(v < INT32_MIN || v > INT32_MAX){
        return false;
    }
    out = (int32_t)v;
    return true;
}
static Variable *findvariable(Frame &f, const string& name){ //Finds a variable by name inside one frame
    for(int32_t i = 0; i < f.argc; i++){
        if(f.argv[i].name == name){
            return &f.argv[i];
        }
    }
    for(int32_t i = 0; i < f.localCount; i++){
        if(f.locals[i].name == name){
            return &f.locals[i];
        }
    }
    return nullptr;
}
static bool getValue(Frame &f,const string& tok,int32_t& out, string& err){ // Gets the value of an operand
    if(IntegerParse(tok, out)){
        return true;
    }
    Variable* v = findvariable(f, tok);
    if( v == nullptr){
        err = "unknown variable" + tok + " ";
        return false;
    }
    out = v->value;
    return true;
}
static bool operationapp(const string &op, int32_t a, int32_t b, int32_t &out, string &err){ // does the arithmetic safely in 64 bit operations
    long long r;
    if (op == "add"){
        r = (long long)a + b;
    }
    if (op == "sub"){
        r = (long long)a - b;
    }
    if (op == "mul"){
        r = (long long)a * b;
    }
    else{
        if(b == 0){
            err = "division by zero";
            return false;
        }
        r = (long long)a / b;
    }
    if( r < INT32_MIN || r > INT32_MAX){
        err = "integer overflow";
        return false;
    }
    out = (int32_t)r;
    return true;
}
void executeProgram(const char *resolveBinPath, int64_t mainOffset, Timeline &timeline)
{
    FILE *f = fopen(resolveBinPath, "rb");
    if(!f){
        cerr << "Execution error: cannot open" << resolveBinPath << endl;
    }
    Stack <Frame> callStack;
    string argSource[MAX_STACK_DEPTH][MAX_VARS_PER_FRAME];
    // Push Frame for main
    string text;
    if(fseek(f,(long)mainOffset, SEEK_SET) != 0 || readResolveRecord(f, text) < 0){
        cerr << "Main  not found in this program" << resolveBinPath << endl;
        fclose(f);
        return;
    }

    Frame mainFrame{};
    mainFrame.func_name = "main";
    mainFrame.argc = 0;
    mainFrame.returnLine = -1;
    mainFrame.localCount = 0;
    callStack.push(mainFrame);
    int32_t steps = 0;
    bool done = false;
    while(!done){
        int64_t offsetfield = readResolveRecord(f, text);
        if(offsetfield < 0){
            cerr << "error : ran past the end of the program" << endl;
            break;
        }
        Token currtokens[MAX_TOKENS];
        int32_t n = tokenizeLine(text, currtokens, MAX_TOKENS);
        if( n < 1){
            cerr << "Runtime error: too many words" << endl;
            break;
        }
        Frame& curr = callStack.peek();
        const string& kw = currtokens[0].text;
        string err;
        // if kw is a set statement;
        if(kw == "set"){
            int32_t tmp, v = 0;
            if(n != 3){
                err =  "error: set needs to have exactly 3 words";
            }
            else if(IntegerParse(currtokens[1].text,tmp)){
                err =  "Cannot assign value to a number" ;
            }
            else if(getValue(curr, currtokens[2].text,v, err)){
                Variable* ifexist = findvariable(curr, currtokens[1].text);
                if(ifexist != nullptr){ //if local variable alr exists, program will overwrite it
                    ifexist->value = v;
                }
                else if(curr.localCount >= MAX_VARS_PER_FRAME){
                    err = "too many local variables";
                }
                else{
                    curr.locals[curr.localCount].name = currtokens[1].text;
                    curr.locals[curr.localCount].value = v;
                    curr.localCount++;
                }
            }
        } //if arithmetic operation is being done
        else if( kw == "add" || kw == "sub" || kw == "mul" || kw == "div"){
            int32_t right = 0, result = 0;
            if(n != 3){
                err =  kw + " needs 2 operators" ;
            }
            else {
                Variable* ifexist = findvariable(curr, currtokens[1].text);
                if(ifexist == nullptr){
                    err = "unknown variable" + currtokens[1].text  + "' ";
                }
                else if(getValue(curr,currtokens[2].text,right,err) && operationapp(kw, ifexist->value,right, result,err)){
                    ifexist->value = result;
                }
            }
        }
        else if(kw == "call"){
            if(n < 2){
                err = "call does not have a function name";
            }
            else if(callStack.depth() >= MAX_STACK_DEPTH){
                err = "call stack overflow(stack more than 64 calls deep)";
            }
            else{
                int32_t nargs = n-2;
                int64_t returnpos = ftell(f);
                string Headertext;
                if(fseek(f,(long)offsetfield, SEEK_SET)!= 0 || readResolveRecord(f, Headertext) < 0){
                    err = "cannot read function Headertext";
                }
                else{
                    Token hdr[MAX_TOKENS];
                    int32_t hn = tokenizeLine(Headertext,hdr, MAX_TOKENS);
                    cerr << "DBG line: [" << text << "] depth=" << callStack.depth() << endl;
                    if(hn < 2){
                        err = "bad function header";
                    }
                    else if(hn -2 != nargs){
                         err = "'" + currtokens[1].text + "' expects " + to_string(hn - 2) + " argument(s) but got " + to_string(nargs);
                    }
                    else{
                        Frame callee{};
                        callee.func_name = currtokens[1].text;
                        callee.argc = nargs;
                        callee.returnLine = (int32_t)returnpos;
                        callee.localCount = 0;
                        int32_t level = callStack.depth();
                        for (int32_t i = 0; i < nargs; i++)
                        {
                            int32_t v = 0, dummy;
                            if (!getValue(curr, currtokens[2 + i].text, v, err))
                                break;
                            callee.argv[i].name = hdr[2 + i].text;
                            callee.argv[i].value = v;
                            argSource[level][i] = IntegerParse(currtokens[2 + i].text, dummy) ? "" : currtokens[2 + i].text;
                        }
                        if (err.empty()){
                            callStack.push(callee);
                        }
                    }
                }
            }
        }
        else if(kw == "func_end"){
            Frame finished = callStack.pop();
            if(callStack.isEmpty()){
                done = true;                       
            }
            else{
                Frame &caller = callStack.peek();
                int32_t level = callStack.depth(); 
                for(int32_t i = 0; i < finished.argc; i++){
                    if(!argSource[level][i].empty()){
                        Variable *v = findvariable(caller, argSource[level][i]);
                        if(v != nullptr) v->value = finished.argv[i].value;
                    }
                }
                fseek(f, (long)finished.returnLine, SEEK_SET);   
            }
        }
        else if(kw == "func"){
            // nothing to do
        }
        else{
            err = "unknown instruction '" + kw + "'";
        }

        if(!err.empty()){
            cerr << "Runtime error at step " << steps + 1 << " ('" << text << "'): " << err << endl;
            break;
        }

        timeline.record(buildSnapshot(callStack));   // <-- the line you were looking for
        steps++;
        if(!done && steps >= MAX_STEPS){
            cerr << "Stopped: reached the limit of " << MAX_STEPS << " steps" << endl;
            break;
        }
    }   // end of while

    fclose(f);
}
    // initialize the call stack
    // make the main frame
    // push main frame on the call stack

    // implementation:
    // execute line by line, and according to the keyword perform action


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
// /home/nashwa_khan/ttdb/server.cpp
int32_t main()
{
    if (!validateProgram("source.bin"))
    {
        return 1;
    }

    int64_t mainOffset = resolveProgram("source.bin", "resolve.bin");
    if (mainOffset < 0)
    {
        return 1;
    }

    Timeline timeline;
    executeProgram("resolve.bin", mainOffset, timeline);
    cout << "Recorded " << timeline.getStepCount() << " snapshots" << endl;

    int32_t n = 1;
    for (TimelineNode *node = timeline.begin(); node != nullptr; node = node->next, n++)
    {
        Snapshot *s = node->data;
        cout << "Snapshot " << n << " (depth " << s->stackDepth << ")" << endl;
        for (int32_t i = 0; i < s->stackDepth; i++)
        {
            Frame &fr = s->callStack[i];
            cout << "  " << fr.func_name << " | args:";
            for (int32_t j = 0; j < fr.argc; j++)
                cout << " " << fr.argv[j].name << "=" << fr.argv[j].value;
            cout << " | locals:";
            for (int32_t j = 0; j < fr.localCount; j++)
                cout << " " << fr.locals[j].name << "=" << fr.locals[j].value;
            cout << endl;
        }
    }
    return 0;
}