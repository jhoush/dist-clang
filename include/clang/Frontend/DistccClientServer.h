//===-- DistccClientServer.h ----------------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "clang/Frontend/CompilerInstance.h"
#include "clang/Basic/Diagnostic.h"

#include <queue>

// FIXME: Stop using zmq
#include <zmq.hpp>

namespace clang {
class DistccClientServer { 
private:	
    // Custom DiagnosticClient necessary for using StoredDiagnostic
    // which includes a built-in serialization function
    // see FIXME: ActualHeaderFile.h
    class StoredDiagnosticClient : public DiagnosticClient {
        llvm::SmallVector<StoredDiagnostic,4> &StoredDiags;
  
        public:
        explicit StoredDiagnosticClient(llvm::SmallVector<StoredDiagnostic,4>
                                        &StoredDiags)
           : StoredDiags(StoredDiags) { }
   
        void HandleDiagnostic(Diagnostic::Level Level,
                              const DiagnosticInfo &Info) {
            StoredDiags.push_back(StoredDiagnostic(Level,Info));
        }
    };


    // Struct to handle work data
    struct CompilerWork {
        CompilerWork(uint64_t id, std::string a, std::string s)
            : uniqueID(id), args(a) ,source(s){}
        
        uint64_t uniqueID;
        std::string args;
        std::string source;
    }; 
    
    // Local vars 
    std::queue<CompilerWork> workQueue; // queue holding all the assigned work
    pthread_mutex_t workQueueMutex; // mutex protecting the work queue
    pthread_cond_t  recievedWork;
    pthread_cond_t  noWork;
    zmq::context_t zmqContext;
    
    // Helpful threads
    pthread_t requestThread;
    pthread_t compilerThread;
    pthread_t supplicationThread;
    pthread_t delegateThread;
    
    void *RequestThread();
    void *CompilerThread();
    void *SupplicationThread();
    void *DelegateThread();
    
    CompilerWork ProcessMessage(zmq::message_t& msg);
    std::string ProcessCompilerWork(CompilerWork& work);
    
    // Status variables for threads
    void *requestThreadStatus;
    void *compilerThreadStatus;
    void *supplicationThreadStatus;
    void *delegateThreadStatus;
    
    // Boostrapping functions for pthreads
    static void *pthread_RequestThread(void *ctx);
    static void *pthread_CompilerThread(void *ctx);
    static void *pthread_SupplicationThread(void *ctx);
    static void *pthread_DelegateThread(void *ctx);
    
    // FIXME: Remove this copied(from Driver.cpp) when becomes available
    // in sys::Path
    std::string GetTemporaryPath(const char *Suffix) const;
public:
    DistccClientServer();
    ~DistccClientServer(){}
};
}
