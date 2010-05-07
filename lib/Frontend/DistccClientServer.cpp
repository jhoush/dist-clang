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

#include "clang/Frontend/CodeGenAction.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/FileManager.h"
#include "clang/Basic/TargetInfo.h"

#include "llvm/Support/MemoryBuffer.h"
#include "llvm/LLVMContext.h"

// FIXME: Replace UNIX-specific operations with system-agnostc ones
#include <pthread.h>

// FIXME: stop using zmq
#undef error_t
#include <zmq.hpp>

// FIXME: remove these, for testing only
#include <iostream>
#include <fstream>

using namespace clang;

DistccClientServer::DistccClientServer() {
    // initialize synchronization tools
    pthread_mutex_init(&workQueueMutex, NULL);
    pthread_cond_init(&recievedWork, NULL);

    llvm::errs() << "Created a client server.\n";
    llvm::errs().flush();
    
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
	args.push_back("-o");
	args.push_back("foo.o");
	int len;
	char* tmp = Distcc::serializeArgVector(args, len);
	std::string sArgs(tmp, len);
	free(tmp);
	CompilerWork work = CompilerWork(sArgs, source);
    pthread_mutex_lock(&workQueueMutex);
    workQueue.push(work);
    pthread_cond_signal(&recievedWork);
    pthread_mutex_unlock(&workQueueMutex);
    llvm::errs().flush();
    
    // loop
    while (1) {
        break;
        //int lengthRead = 0;
        //int sizeOfArgs = 0;
        // recieve length of args
        /*if ((lengthRead = revc(masterFd, &sizeOfArgs, sizeof(sizeOfArgs), MSG_WAITALL)) < (int)sizeof(sizeOfArgs)) {
            llvm::errs() << "error recieving size of args from server" << lengthRead << "\n";
            close(masterFd);
            return;
        }*/

        // recieve args
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
    
    return NULL; // suppress warning
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
        CompilerWork work = workQueue.front();
        workQueue.pop();
        pthread_mutex_unlock(&workQueueMutex);
        llvm::errs() << "retrieved work from queue\n";

        llvm::StringRef Source(work.source);
        std::vector<std::string> args = Distcc::deserializeArgVector((char*)work.args.data(), (int) work.args.size());
		llvm::SmallVector<const char *, 32> argAddresses;
		for (unsigned i=0;i<args.size();i++){
		    if (args[i] == "-o") {
                i += 2;
                continue;
		    }
		    
			argAddresses.push_back(args[i].c_str());
		}
		llvm::errs() << "deserialized args\n";


		// setup compiler instance
		llvm::SmallVectorImpl<StoredDiagnostic> StoredDiags(10);
		StoredDiagnosticClient DiagsBuffer(StoredDiags);
		CompilerInvocation Invocation;
		CompilerInstance Clang;
        Clang.setDiagnosticClient(&DiagsBuffer);
        Diagnostic *Diags = new Diagnostic(&DiagsBuffer);
        Clang.setDiagnostics(Diags);
		CompilerInvocation::CreateFromArgs(Invocation, (const char **)argAddresses.begin(),
										   (const char **)argAddresses.end(), Clang.getDiagnostics());
		Clang.setInvocation(&Invocation);
		llvm::errs() << "set up compiler instance\n";

        // tweak frontend options
        FrontendOptions& FeOpts = Clang.getFrontendOpts();
        FeOpts.ProgramAction = frontend::EmitObj;
        std::vector<std::pair<FrontendOptions::InputKind, std::string> > opts;
        std::pair<FrontendOptions::InputKind, std::string> p(FeOpts.IK_PreprocessedCXX, "source");
        opts.push_back(p);
        FeOpts.Inputs = opts;
        llvm::errs() << "tweaked frontend options\n";
    
		Clang.createFileManager();
		Clang.createSourceManager();
        Clang.setTarget(TargetInfo::CreateTargetInfo(Clang.getDiagnostics(),
                                                     Clang.getTargetOpts()));
        Clang.getTarget().setForcedLangOptions(Clang.getLangOpts());

        // setup source file
        llvm::MemoryBuffer* Buffer = llvm::MemoryBuffer::getMemBuffer(Source, "source");
        SourceManager& SM = Clang.getSourceManager();
        FileManager& FM = Clang.getFileManager();
        std::string sourceName("source");
        const FileEntry* fe = FM.getVirtualFile(sourceName, strlen(Buffer->getBufferStart()), 0);//time(NULL));
        SM.overrideFileContents(fe, Buffer);
        llvm::errs() << "overrode source file\n";

        Clang.createPreprocessor();
		llvm::LLVMContext llvmc;
		Clang.setLLVMContext(&llvmc);

        // compile
        llvm::errs() << "start compilation\n";
        //EmitObjAction E;
        EmitAssemblyAction E;
        E.BeginSourceFile(Clang, sourceName);
        E.Execute();

        // get the object code
        const llvm::MemoryBuffer* mb = SM.getMemoryBufferForFile(FM.getFile("source.s"));
        std::string objectCode = mb->getBuffer();
        Clang.clearOutputFiles(true);
		E.EndSourceFile();
        llvm::errs() << "finished compilation\n";

        // FIXME: remove, for test only
        std::string errName("foo");
        llvm::raw_fd_ostream f("a.out", errName, (unsigned int) 0);
        f << objectCode;
        f.close();
        llvm::errs() << "wrote output file\n";
        
        // avoid calling destructors twice
        Clang.takeDiagnosticClient();
        Clang.takeInvocation();
        Clang.takeLLVMContext();
                
        break;
    }
    llvm::errs() << "compiler thread: exit\n";
    llvm::errs().flush();
        
    return NULL;  // suppress warning
}

// Static pthread helper method
void *DistccClientServer::pthread_RequestThread(void *ctx){
    return ((DistccClientServer *)ctx)->RequestThread();
}

void *DistccClientServer::pthread_CompilerThread(void *ctx){
    return ((DistccClientServer *)ctx)->CompilerThread();
}

DistccClientServer::~DistccClientServer(){}
