//===-- DistccClientServer.h ----------------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "clang/Frontend/CompilerInstance.h"
#include "llvm/System/Mutex.h"
#include "llvm/System/Path.h"
#include <queue>
#include <string>

namespace clang {
class DistccClientServer { 
private:	
    // struct to handle work data
    struct CompilerWork {
        CompilerWork(char* a, char* s) {
            args = a;
            source = s; 
        }
        
        ~CompilerWork() {
            /*
            llvm::errs() << "freeing args\n";
            delete args;
            llvm::errs() << "freeing source\n";
            delete source;
            llvm::errs().flush();
            */
        }
        
        char* args;
        char* source;
    };
    
    // local vars 
    std::queue<CompilerWork> *workQueue; // queue holding all the assigned work
    llvm::sys::Mutex workQueueMutex;          // mutex protecting the work queue
    CompilerInstance *CI;
    
    pthread_t requestThread;
    pthread_t compilerThread;
    
    void *RequestThread();
    void *CompilerThread();
    
    // Boostrapping functions for pthreads
    static void *pthread_RequestThread(void *ctx);
    static void *pthread_CompilerThread(void *ctx);
    
    void startClientServer();
    
public:
    DistccClientServer(clang::CompilerInstance &CI);
    ~DistccClientServer();
    
};
}
