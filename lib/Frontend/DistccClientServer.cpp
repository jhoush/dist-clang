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
#include <zmq.hpp>

// FIXME: remove these, for testing only
#include <iostream>
#include <fstream>

using namespace clang;

DistccClientServer::DistccClientServer()
: zmqContext(2, 2) {
    master = new zmq::socket_t(zmqContext, ZMQ_P2P);
	master->bind("tcp://127.0.0.1:5555");
    
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
	CompilerWork work = CompilerWork(0, sArgs, source);
    pthread_mutex_lock(&workQueueMutex);
    workQueue.push(work);
    pthread_cond_signal(&recievedWork);
    pthread_mutex_unlock(&workQueueMutex);
    llvm::errs().flush();
    
    while (1) {
        break;
        uint64_t uniqueID;
        uint32_t argLen;
        zmq::message_t msg;
        
        // receive args
        if (master->recv(&msg) < 0) {
            // handle an error
        }
        
        // process message
        char *msgData = (char *)msg.data();
        int offset = 0;
        memcpy(&uniqueID, &msgData[offset], sizeof(uniqueID));
        offset += sizeof(uniqueID);
        memcpy(&argLen, &msgData[offset], sizeof(argLen));
        
        // copy args
        offset += sizeof(argLen);
        char *tmp = new char[argLen];
        memcpy(tmp, &msgData[offset], argLen);
        std::string args((const char *) tmp, argLen);
        delete tmp;
        
        // copy source
        offset += argLen;
        int sourceSize = msg.size() - offset;
        tmp = new char[sourceSize];
        memcpy(tmp, &msgData[offset], sourceSize);
        std::string source((const char *) tmp, sourceSize);
        delete tmp;
        
        delete msgData;

        // add to work queue
        pthread_mutex_lock(&workQueueMutex);
        workQueue.push(CompilerWork(uniqueID, args, source));
        pthread_cond_signal(&recievedWork);
        pthread_mutex_unlock(&workQueueMutex);

        llvm::errs() << "added work to queue\n";
    }
        
    llvm::errs() << "request thread: exit\n";
    llvm::errs().flush();
    
    return NULL; // suppress warning
}

void *DistccClientServer::CompilerThread() {
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

        uint64_t uniqueID = work.uniqueID;
        llvm::StringRef Source(work.source);
        std::vector<std::string> args = Distcc::deserializeArgVector((char*)work.args.data(),
                                                                     (int) work.args.size());
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
		
		Clang.createFileManager();
		Clang.createSourceManager();
		
		
		// setup source file
        llvm::MemoryBuffer* Buffer = llvm::MemoryBuffer::getMemBuffer(Source);
        SourceManager& SM = Clang.getSourceManager();
		
		llvm::StringRef dummyPath("<dummy file>");
		const FileEntry *FE = Clang.getFileManager().getVirtualFile(dummyPath, Buffer->getBufferSize(), 0);
		SM.overrideFileContents(FE, Buffer);

		
		//Create stream to send object code to
		llvm::StringRef emptyPath("");
		std::string objectCode; 
		Clang.clearOutputFiles(false);
		//FIXME: use .take()?
		llvm::raw_string_ostream objectCodeStream(objectCode);
		
		llvm::errs() << "set up compiler instance\n";
    
        Clang.setTarget(TargetInfo::CreateTargetInfo(Clang.getDiagnostics(),
                                                     Clang.getTargetOpts()));
        Clang.getTarget().setForcedLangOptions(Clang.getLangOpts());
		
		Clang.createPreprocessor();

        llvm::errs() << "overrode source file\n";

		llvm::LLVMContext llvmc;
		Clang.setLLVMContext(&llvmc);
		
		
        // compile
        llvm::errs() << "start compilation\n";
        EmitObjAction E;
		
		//FIXME: Remove this ugly hack to get output to redirect to string
		//FIXME: use .take()?		
		E.setOutputStream(&objectCodeStream);
		
		E.BeginSourceFile(Clang, dummyPath);
		
        E.Execute();

        // get the object code
		E.EndSourceFile();

        llvm::errs() << "finished compilation\n";

		llvm::errs() << "Object code size: " << objectCode.size() << "\n";
		
        // avoid calling destructors twice
        Clang.takeDiagnosticClient();
        Clang.takeInvocation();
        Clang.takeLLVMContext();
        

        // serialize diagnostics
        std::string diags;
        llvm::raw_string_ostream diagsStream(diags);
        llvm::SmallVectorImpl<StoredDiagnostic>::iterator diagIterator = StoredDiags.begin();
        while (diagIterator != StoredDiags.end()) {
            diagIterator->Serialize(diagsStream); // might work
            ++diagIterator;
        }
		
		uint32_t diagLen = diags.size();
		// create message
        uint32_t totalLen = sizeof(uniqueID) + sizeof(diagLen) +
		diags.size() + objectCode.size();
        zmq::message_t msg(totalLen);
        char *offset = (char *)msg.data();
        memcpy(offset, &uniqueID, sizeof(uniqueID));
        offset += sizeof(uniqueID);
        memcpy(offset, &diagLen, sizeof(diagLen));
        offset += diagLen;		
        
        memcpy(offset, diags.data(), diagLen);
        offset += diagLen;
        memcpy(offset, objectCode.c_str(), objectCode.size());
        
        master->send(msg);
        
        
        delete offset;
        
        break;
    }
    llvm::errs() << "compiler thread: exit\n";
    llvm::errs().flush();
        
    return NULL; // suppress warning
}

// Static pthread helper method
void *DistccClientServer::pthread_RequestThread(void *ctx){
    return ((DistccClientServer *)ctx)->RequestThread();
}

void *DistccClientServer::pthread_CompilerThread(void *ctx){
    return ((DistccClientServer *)ctx)->CompilerThread();
}

DistccClientServer::~DistccClientServer(){
	delete master;
}
