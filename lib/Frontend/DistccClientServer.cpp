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
//#include "llvm/ADT/OwningPtr.h"
//#include "clang/Driver/Compilation.h"
//#include "clang/Driver/Driver.h"
//#include "llvm/System/Host.h"
//#include "clang/Frontend/Utils.h"

#include "clang/Basic/SourceManager.h"
#include "llvm/Support/MemoryBuffer.h"
#include "clang/Frontend/CodeGenAction.h"
#include "clang/Basic/FileManager.h"
#include "clang/Basic/TargetInfo.h"

#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/System/Path.h"
#include "llvm/LLVMContext.h"

// FIXME: Replace UNIX-specific operations with system-agnostc ones
#include <pthread.h>
#include <iostream>
#include <fstream>
#include <time.h>

// FIXME: stop using zmq

using namespace clang;
//using namespace clang::driver;

DistccClientServer::DistccClientServer(CompilerInstance &CI) {
    this->CI = &CI;

    // initialize synchronization tools
    pthread_mutex_init(&workQueueMutex, NULL);
    pthread_cond_init(&recievedWork, NULL);

    llvm::errs() << "Created a client server.\n";
    llvm::errs().flush();
    
    startClientServer();
}

void DistccClientServer::startClientServer() {
    llvm::errs() << "workqueue size " << workQueue.size() << "\n";
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
    void* status;
    pthread_join(compilerThread, &status);
    pthread_join(requestThread, &status);
}

void *DistccClientServer::RequestThread() {
    // FIXME: remove this, for test only
    std::ifstream s("test2.c");
    std::string source = "";
    std::string line;
    if (s.is_open()) {
        while (! s.eof() ){
            getline(s,line);
            source += line + "\n";
        }
        s.close();
    }
	std::vector<std::string> args = std::vector<std::string>();
	args.push_back("test2.c");
	args.push_back("-resource-dir");
	args.push_back("/home/joshua/llvm/Debug/lib/clang/1.5");
	args.push_back("-fdollars-in-identifiers");
	args.push_back("-fno-operator-names");
	args.push_back("-ftemplate-depth");
	args.push_back("99");
	args.push_back("-triple");
	args.push_back("x86_64-unknown-linux-gnu");
	args.push_back("-target-feature");
	args.push_back("-sse41");
	args.push_back("-target-feature");
	args.push_back("-ssse3");
	args.push_back("-target-feature");
	args.push_back("+mmx");
	args.push_back("-target-feature");
	args.push_back("-sse42");
	args.push_back("-target-feature");
	args.push_back("-aes");
	args.push_back("-target-feature");
	args.push_back("-3dnow");
	args.push_back("-target-feature");
	args.push_back("-3dnowa");
	args.push_back("-target-feature");
	args.push_back("+sse2");
	args.push_back("-target-feature");
	args.push_back("+sse");
	args.push_back("-target-feature");
	args.push_back("-sse3");
	int len;
	char* tmp = Distcc::serializeArgVector(args, len);
	std::string sArgs(tmp, len);
	free(tmp);
	CompilerWork work = CompilerWork(sArgs, source);
    pthread_mutex_lock(&workQueueMutex);
    llvm::errs() << "pushing work " << workQueue.size() << "\n";
    workQueue.push(work);
    pthread_cond_signal(&recievedWork);
    pthread_mutex_unlock(&workQueueMutex);
    llvm::errs().flush();
    
    // loop
    while (1) {
        break;
    /*
        int lengthRead = 0;
        int sizeOfArgs = 0;
        // recieve length of args
        if ((lengthRead = revc(masterFd, &sizeOfArgs, sizeof(sizeOfArgs), MSG_WAITALL)) < (int)sizeof(sizeOfArgs)) {
            llvm::errs() << "error recieving size of args from server" << lengthRead << "\n";
            close(masterFd);
            return;
        }

        // recieve args 
*/
        llvm::errs() << "got args from master\n";
        
        // get source from master

        llvm::errs() << "got source from master\n";
        // FIXME: remove the next two lines
        char* args = new char();
        char* source = new char();
        
        pthread_mutex_lock(&workQueueMutex);
        workQueue.push(CompilerWork(args, source));
        pthread_cond_signal(&recievedWork);
        pthread_mutex_unlock(&workQueueMutex);

        llvm::errs() << "added work to queue\n";
    }
        
    llvm::errs() << "request thread: exit\n";
    llvm::errs().flush();
    
    return NULL; // FIXME: shouldn't actually return this
}

void *DistccClientServer::CompilerThread() {
    // loop
    // FIXME: should timeout at some point
    while (1) {
        // grab from workQueue
        pthread_mutex_lock(&workQueueMutex);
        while (workQueue.size() == 0) {
            pthread_cond_wait(&recievedWork, &workQueueMutex);
        }
        llvm::errs() << workQueue.size() << "\n";
        CompilerWork work = workQueue.front();
        workQueue.pop();
        pthread_mutex_unlock(&workQueueMutex);
        llvm::errs() << "retrieved work from queue\n";

        llvm::StringRef Source(work.source);
        std::vector<std::string> args = Distcc::deserializeArgVector((char*)work.args.data(), (int) work.args.size());
		llvm::SmallVector<const char *, 32> argAddresses;
		for(unsigned i=0;i<args.size();i++){
			argAddresses.push_back(args[i].c_str());
			//llvm::errs() << args[i] << "\n";
		}
		llvm::errs() << "deserialized args\n";

		CompilerInstance Clang;
        TextDiagnosticBuffer DiagsBuffer;
		CompilerInvocation Invocation;
		Diagnostic Diags(&DiagsBuffer);
		
        Clang.setDiagnostics(&Diags);
        Clang.setDiagnosticClient(&DiagsBuffer);
		CompilerInvocation::CreateFromArgs(Invocation, (const char **)argAddresses.begin(),
										   (const char **)argAddresses.end(), Diags);
		Clang.setInvocation(&Invocation);
		llvm::errs() << "set up compiler instance\n";
		
        llvm::errs() << "checkpoint 1\n";
		Clang.createFileManager();
        llvm::errs() << "checkpoint 2\n";
        Clang.setTarget(TargetInfo::CreateTargetInfo(Clang.getDiagnostics(),
                                                     Clang.getTargetOpts()));
        llvm::errs() << "checkpoint 3\n";
        Clang.getTarget().setForcedLangOptions(Clang.getLangOpts());
        llvm::errs() << "checkpoint 4\n";
        
        // override the source file
        llvm::MemoryBuffer* Buffer = llvm::MemoryBuffer::getMemBuffer(Source, "source");
		llvm::errs() << "checkpoint 5\n";

        SourceManager SM(Diags);
        Clang.createFileManager();
        FileManager& FM = Clang.getFileManager();
        std::string sourceName("source");
        const FileEntry* fe = FM.getVirtualFile(sourceName, strlen(Buffer->getBufferStart()), time(NULL));
        SM.overrideFileContents(fe, Buffer);
        llvm::errs() << "overrode source file\n";
        
		Clang.setSourceManager(&SM);
        Clang.createPreprocessor();
		llvm::LLVMContext llvmc;
		Clang.setLLVMContext(&llvmc);

        // set the output file
        Clang.clearOutputFiles(false);
        std::string objectCode;
        llvm::raw_string_ostream* OS = new llvm::raw_string_ostream(objectCode);
		llvm::errs() << "checkpoint 6\n";
        llvm::sys::Path Path = llvm::sys::Path::GetCurrentDirectory();
        llvm::StringRef PathName(Path.getBasename());
		llvm::errs() << "checkpoint 7\n";
        Clang.addOutputFile(PathName, OS);
        llvm::errs() << "set output file\n";

        // compile
        llvm::errs() << "start compilation\n";
        EmitObjAction E;
        E.BeginSourceFile(Clang, sourceName, false);
        E.Execute();
		E.EndSourceFile();
        llvm::errs() << "finished compilation\n";
        
        // FIXME: remove, for test only
        std::string fname("a.out");
        std::string errName("foo");
        llvm::raw_fd_ostream f(fname.c_str(), errName, (unsigned int) 0);
        f << objectCode;
        f.close();
        llvm::errs() << "wrote output file\n";
                
        break;
    }
    llvm::errs() << "compiler thread: exit\n";
    llvm::errs().flush();
        
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
