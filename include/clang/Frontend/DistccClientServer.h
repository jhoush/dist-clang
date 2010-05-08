//===-- DistccClientServer.h ----------------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "clang/Frontend/CompilerInstance.h"
#include "llvm/System/Path.h"
#include "clang/Basic/Diagnostic.h"

#include <queue>
#include <string>

// FIXME: Stop using zmq
#include <zmq.hpp>

namespace clang {
class DistccClientServer { 
private:	

    class StoredDiagnosticClient : public DiagnosticClient {
        llvm::SmallVectorImpl<StoredDiagnostic> &StoredDiags;
  
        public:
        explicit StoredDiagnosticClient(llvm::SmallVectorImpl<StoredDiagnostic> &StoredDiags)
           : StoredDiags(StoredDiags) { }
   
        void HandleDiagnostic(Diagnostic::Level Level, const DiagnosticInfo &Info) {
            StoredDiags.push_back(StoredDiagnostic(Level, Info));
        }
    };


    // struct to handle work data
    struct CompilerWork {
        CompilerWork(uint64_t id, std::string a, std::string s)
                     : uniqueID(id), args(a) ,source(s){}
        
        uint64_t uniqueID;
        std::string args;
        std::string source;
    }; 
    
    // local vars 
    std::queue<CompilerWork> workQueue; // queue holding all the assigned work
    pthread_mutex_t workQueueMutex; // mutex protecting the work queue
    pthread_cond_t  recievedWork;
    zmq::socket_t *master;
    zmq::context_t zmqContext;
    
    pthread_t requestThread;
    pthread_t compilerThread;
    
    void *RequestThread();
    void *CompilerThread();
    
    // Boostrapping functions for pthreads
    static void *pthread_RequestThread(void *ctx);
    static void *pthread_CompilerThread(void *ctx);
    
    void startClientServer();
    
public:
    DistccClientServer();
    ~DistccClientServer();
    
};
}
