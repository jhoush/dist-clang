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

using namespace clang;

DistccClientServer::DistccClientServer()
: zmqContext(2, 1) {
  // initialize synchronization tools
  pthread_mutex_init(&workQueueMutex, NULL);
  pthread_cond_init(&recievedWork, NULL);
  
  llvm::errs() << "Starting request and compiler threads\n";
  llvm::errs().flush();
	
  if (pthread_create(&requestThread, NULL,
                     pthread_RequestThread, this) < 0) {
    llvm::errs() << "Error creating request thread\n";
    llvm::errs().flush();
  }
  
  if (pthread_create(&compilerThread, NULL,
                     pthread_CompilerThread, this) < 0) {
    llvm::errs() << "Error creating compiler thread\n";
    llvm::errs().flush();
  }
  while(1);

  // TODO: something more with these statuses?
  pthread_join(compilerThread, &compilerThreadStatus);
  pthread_join(requestThread,  &requestThreadStatus);
  llvm::errs().flush();
}

void *DistccClientServer::RequestThread() {
  zmq::socket_t master(zmqContext, ZMQ_UPSTREAM);
  // FIXME: remove hardcoded address
  master.bind("tcp://127.0.0.1:5555");

  // FIXME: should timeout at some point    
  while (1) {
    llvm::errs() << "Waiting for more work\n";
    uint64_t uniqueID;
    uint32_t argLen;
    zmq::message_t msg;
    
    // receive args
    if (master.recv(&msg) < 0) {
        llvm::errs() << "err receiving message\n";
        // handle an error
    }
    
    llvm::errs() << "got message\n";
    
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
    
    // FIXME: remove
    break;
  }
      
  llvm::errs() << "request thread: exit\n";
  llvm::errs().flush();

  return NULL; // suppress warning
}

void *DistccClientServer::CompilerThread() {
  zmq::socket_t master(zmqContext, ZMQ_DOWNSTREAM);
  // FIXME: remove hardcoded address
  master.connect("tcp://localhost:5556");

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
    char *serializedArgs = (char*)work.args.data();
    int argSize = work.args.size();
    std::vector<std::string> args = Distcc::deserializeArgVector(serializedArgs,
                                                                 argSize);
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
    
    const char **ArgBegin = (const char **)argAddresses.begin();
    const char **ArgEnd = (const char **)argAddresses.end();
    CompilerInvocation::CreateFromArgs(Invocation, ArgBegin,
                       ArgEnd, Clang.getDiagnostics());
    Clang.setInvocation(&Invocation);

    Clang.createFileManager();
    Clang.createSourceManager();


    // setup source file
    llvm::MemoryBuffer* Buffer = llvm::MemoryBuffer::getMemBuffer(Source);
    SourceManager& SM = Clang.getSourceManager();

    llvm::StringRef dummyPath("<dummy file>");
    size_t BufSize = Buffer->getBufferSize();
    const FileEntry *FE = Clang.getFileManager().getVirtualFile(dummyPath,
                                                               BufSize, 0);
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
    llvm::SmallVectorImpl<StoredDiagnostic>::iterator itr = StoredDiags.begin();
    while (itr != StoredDiags.end()) {
      itr->Serialize(diagsStream); // might work
      ++itr;
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
    
    master.send(msg);
    
    
    delete offset;
    
    // FIXME: remove
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
