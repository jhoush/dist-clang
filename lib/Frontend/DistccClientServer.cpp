//===-- DistccClientServer.cpp --------------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "clang/Frontend/DistccClientServer.h"
#include "clang/Frontend/Distcc.h"
#include "clang/Frontend/TextDiagnosticBuffer.h"
#include "clang/Frontend/TextDiagnosticPrinter.h"
#include "llvm/ADT/OwningPtr.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/Driver/Compilation.h"
#include "clang/Driver/Driver.h"
#include "llvm/System/Host.h"
#include "clang/Frontend/Utils.h"

// FIXME: Replace UNIX-specific operations with system-agnostc ones
#include <pthread.h>


using namespace clang;
using namespace clang::driver;

DistccClientServer::DistccClientServer(CompilerInstance &CI) {
    this->CI = &CI;
    llvm::errs() << "Created a client server.\n";
    llvm::errs().flush();
    
    workQueue = new std::queue<CompilerWork>();
    startClientServer();
}

void DistccClientServer::startClientServer() {
    llvm::errs() << "Starting request and compiler threads\n";
	llvm::errs().flush();
    
    if (pthread_create(&requestThread, NULL, pthread_RequestThread, this) < 0) {
        llvm::errs() << "Error creating request thread\n";
        llvm::errs().flush();
    }
    
    if (pthread_create(&compilerThread, NULL, pthread_CompilerThread, this) < 0) {
        llvm::errs() << "Error creating compiler thread\n";
        llvm::errs().flush();
    }
    
    pthread_exit(NULL);
}

void *DistccClientServer::RequestThread() {

    
    // loop
    while (1) {
        // get args from master
        
        // get source from master
        
        // FIXME: remove the next two lines
        char* args = new char();
        char* source = new char();
        
        workQueueMutex.acquire();
        workQueue->push(CompilerWork(args, source));
        workQueueMutex.release();
    }
    
    return NULL; // FIXME: shouldn't actually return this
}

void *DistccClientServer::CompilerThread() {
    // loop
    while (1) {
        // grab from workQueue
        workQueueMutex.acquire();
        llvm::errs() << workQueue->size()<< "\n";
        while (workQueue->size() == 0) {
            // FIXME: use condition variables
            workQueueMutex.release();
            usleep(100);
            workQueueMutex.acquire();
        }
        CompilerWork work = workQueue->front();
        workQueue->pop();
        workQueueMutex.release();
        llvm::errs() << "grabbed from work queue\n";
        
        std::vector<std::string> args = Distcc::deserializeArgVector(work.args, sizeof(work.args));
        int argc = args.size();
        const char** argv = (const char**) new char*[argc];
        for (int i = 0; i < argc; ++i) {
            argv[i] = args[i].c_str();
        }
        
        // FIXME: currently write source to a tmp file
        //        read resulting output from the produced file
        //        we should do better
        //
        // write to a file
        // FIXME: should modify to be c/cpp/m/whatever
        std::string fname("tmp.c");
        std::string errName("foo");
        llvm::raw_fd_ostream f(fname.c_str(), errName, (unsigned int) 0);
        f << work.source;
        f.close();
        
        // this code is pretty much lifted from the main function in
        // tools/driver/driver.cpp (lines 191-274)  
        bool CanonicalPrefixes = true;
        for (int i = 1; i < argc; ++i) {
            if (llvm::StringRef(argv[i]) == "-no-canonical-prefixes") {
                CanonicalPrefixes = false;
                break;
            }
        }
        
        llvm::sys::Path Path = llvm::sys::Path::GetCurrentDirectory();

        TextDiagnosticPrinter DiagClient(llvm::errs(), DiagnosticOptions());
        DiagClient.setPrefix(Path.getBasename());

        Diagnostic Diags(&DiagClient);

        #ifdef CLANG_IS_PRODUCTION
            const bool IsProduction = true;
        #  ifdef CLANGXX_IS_PRODUCTION
            const bool CXXIsProduction = true;
        #  else
            const bool CXXIsProduction = false;
        #  endif
        #else
            const bool IsProduction = false;
            const bool CXXIsProduction = false;
        #endif

        Driver TheDriver(Path.getBasename(), Path.getDirname(),
                        llvm::sys::getHostTriple(),
                        "a.out", IsProduction, CXXIsProduction,
                        Diags);
                        
        // Check for ".*++" or ".*++-[^-]*" to determine if we are a C++
        // compiler. This matches things like "c++", "clang++", and "clang++-1.1".
        //
        // Note that we intentionally want to use argv[0] here, to support "clang++"
        // being a symlink.
        llvm::StringRef pname = argv[0];
        std::string ProgName(llvm::sys::Path(pname).getBasename());
        if (llvm::StringRef(ProgName).endswith("++") ||
            llvm::StringRef(ProgName).rsplit('-').first.endswith("++")) {
            TheDriver.CCCIsCXX = true;
            TheDriver.CCCGenericGCCName = "g++";
        }               
        llvm::OwningPtr<Compilation> C;
        C.reset(TheDriver.BuildCompilation(argc, argv));
        
        int Res = 0;
        if (C.get())
            Res = TheDriver.ExecuteCompilation(*C);
        
        // TODO: read in the result file, get Diagnostics
        // can get the result file name by C.getResultFiles()
        // can get the Diagnostics by TheDriver.getDiags()
        
        llvm::errs() << "compiled work\n";
            
        // send back object code
        
        // send back diagnostic messages
        
        llvm::errs() << "sent back object code and diagnostics\n";
        llvm::errs().flush();
    }
    
    return NULL; // FIXME: shouldn't actually return this
}

// Static pthread helper method
void *DistccClientServer::pthread_RequestThread(void *ctx){
    return ((DistccClientServer *)ctx)->RequestThread();
}

void *DistccClientServer::pthread_CompilerThread(void *ctx){
    return ((DistccClientServer *)ctx)->CompilerThread();
}

DistccClientServer::~DistccClientServer(){}
