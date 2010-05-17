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

#include "clang/Driver/Action.h"
#include "clang/Driver/ArgList.h"
#include "clang/Driver/Compilation.h"
#include "clang/Driver/Driver.h"
#include "clang/Driver/Types.h"
#include "clang/Driver/Tool.h"
#include "clang/Frontend/CodeGenAction.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/FileManager.h"
#include "clang/Basic/TargetInfo.h"

#include "../Driver/InputInfo.h"
#include "../Driver/ToolChains.h"
#include "../Driver/Tools.h"

#include "llvm/Support/MemoryBuffer.h"
#include "llvm/LLVMContext.h"
#include <iostream>
#include <fstream>
#include <ctime>
#include <unistd.h>


// FIXME: Replace UNIX-specific operations with system-agnostc ones
#include <pthread.h>

// FIXME: stop using zmq
#include <zmq.hpp>

#define stealWork 0

using namespace clang;
DistccClientServer::DistccClientServer() : zmqContext(4, 4) {
  // initialize synchronization tools
  pthread_mutex_init(&workQueueMutex, NULL);
  pthread_cond_init(&recievedWork, NULL);
  pthread_cond_init(&noWork, NULL);

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
  
  if (stealWork) {
    if (pthread_create(&supplicationThread, NULL,
                       pthread_SupplicationThread, this) < 0) {
      llvm::errs() << "Error creating supplication thread\n";
      llvm::errs().flush();
    }
    
    if (pthread_create(&delegateThread, NULL,
                       pthread_DelegateThread, this) < 0) {
      llvm::errs() << "Error creating delegate thread\n";
      llvm::errs().flush();
    }
    
    pthread_join(supplicationThread, &supplicationThreadStatus);
    pthread_join(delegateThread, &delegateThreadStatus);
  }

  // TODO: something more with these statuses?
  pthread_join(compilerThread, &compilerThreadStatus);
  pthread_join(requestThread, &requestThreadStatus);
  llvm::errs().flush();
}

void *DistccClientServer::RequestThread() {
  zmq::socket_t master(zmqContext, ZMQ_UPSTREAM);
  // FIXME: remove hard coded value
  master.connect("tcp://chromatic-dragon.cs.utexas.edu:5555");

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
  zmq::socket_t master(zmqContext, ZMQ_DOWNSTREAM);
  // FIXME: remove hardcoded address
  master.connect("tcp://chromatic-dragon.cs.utexas.edu:5556");

  // FIXME: should timeout at some point
  while (1) {
    // grab from workQueue
    llvm::errs() << "Waiting for work!\n";
    pthread_mutex_lock(&workQueueMutex);
    while (workQueue.size() == 0) {
      pthread_cond_wait(&recievedWork, &workQueueMutex);
    }
    CompilerWork work = workQueue.front();
    workQueue.pop();

    pthread_mutex_unlock(&workQueueMutex);
    llvm::errs() << "retrieved work from queue\n";
    
    if (workQueue.size() == 0) {
      pthread_cond_signal(&noWork);
    }

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
    llvm::SmallVector<StoredDiagnostic, 4> StoredDiags;
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
#if COMPILING_FOR_OSX
    EmitObjAction E;
#else
    EmitAssemblyAction E;
#endif

    // FIXME: Remove this ugly hack to get output to redirect to string
    // FIXME: use .take()?		
    E.setOutputStream(&objectCodeStream);

    E.BeginSourceFile(Clang, dummyPath);

    E.Execute();

    // get the object code
    E.EndSourceFile();
    objectCodeStream.flush();
// FIXME: Remove this once MC is supported in Linux! (get to work mfleming!)
#if !COMPILING_FOR_OSX
    // Write assembly file out, run assembler, put object code back in place    
    std::string assemblyFile = GetTemporaryPath("S");
    llvm::errs() << "Temp assembly file is " << assemblyFile << "\n";
    TargetOptions &TargetOpts = Clang.getTargetOpts();
    
    std::string objectFile = GetTemporaryPath("o");
    llvm::errs() << "Temp object file is " << objectFile << "\n";
    
    // Write out assembly file
    // FIXME: Use PipeJob instead!
    std::string ErrorInfo;
    raw_fd_ostream assemblyStream(assemblyFile.c_str(), ErrorInfo);
    assemblyStream << objectCode;
    assemblyStream.close();
    
    // FIXME: Remove reference to driver... we shouldn't need all this
    // gunk just to execute the assembler
    driver::Driver TheDriver("clang", "/", TargetOpts.Triple,
                             "a.out", false, false, *Diags);
    
    // Construct Driver, Compilation, and AssembleJobAction, so we can assemble
    // the assembly code from the EmitAssemblyAction.
    //
    // This is extremely ugly, and probably expensive work, but it's not worth
    // worrying about, since this code will be obsolete when MC is supported
    // on all platform.

    const char *HostTriple = TargetOpts.Triple.c_str();
    driver::toolchains::Generic_GCC TC(*TheDriver.GetHostInfo(HostTriple),
                                      llvm::Triple(TargetOpts.Triple));
    driver::tools::gcc::Assemble AssembleTool(TC);
    
    driver::InputArgList *argList = new driver::InputArgList(ArgBegin, ArgEnd);
    // Compilation takes ownership of argList
    driver::Compilation C(TheDriver, TC, argList);
    driver::AssembleJobAction assembleAction(NULL, driver::types::TY_Object);
    
    driver::InputInfo Output(objectFile.c_str(), driver::types::TY_Object,NULL);
    
    driver::InputInfo Input(assemblyFile.c_str(),driver::types::TY_Asm,NULL);
    driver::InputInfoList Inputs;
    Inputs.push_back(Input);
    
    AssembleTool.ConstructJob(C, assembleAction, C.getJobs(), 
                              Output, Inputs, C.getArgsForToolChain(&TC, 0),
                              NULL);
    llvm::errs() << "There are " << C.getJobs().size() << " jobs to run(1)\n";
    const driver::Command *FailingCommand = 0;
    C.ExecuteJob(C.getJobs(), FailingCommand);
    objectCode.clear();
    MemoryBuffer *objectBuffer = llvm::MemoryBuffer::getFile(objectFile);
    objectCode.append(objectBuffer->getBufferStart(),
                      objectBuffer->getBufferSize());
    // Remove temporary files
    std::remove(assemblyFile.c_str());
    std::remove(objectFile.c_str());
#endif
    
    llvm::errs() << "finished compilation\n";
    llvm::errs() << "Object code size: " << objectCode.size() << "\n";

    // avoid calling destructors twice
    Clang.takeDiagnosticClient();
    Clang.takeInvocation();
    Clang.takeLLVMContext();
    

    // serialize diagnostics
    std::string diags("");
    llvm::raw_string_ostream diagsStream(diags);
    //llvm::SmallVector<StoredDiagnostic,4>::iterator itr = StoredDiags.begin();
    /*while (itr != StoredDiags.end()) {
      itr->Serialize(diagsStream); // might work
      ++itr;
    }*/
    
    llvm::errs() << "Serialized diagnostics\n";

    // create message
    uint32_t diagLen = 0;
    uint32_t totalLen = sizeof(uniqueID) + sizeof(diagLen) +
                        diagLen + objectCode.size();
    zmq::message_t msg(totalLen);
    char *offset = (char *)msg.data();
    memcpy(offset, &uniqueID, sizeof(uniqueID));
    offset += sizeof(uniqueID);
    memcpy(offset, &diagLen, sizeof(diagLen));
    offset += sizeof(diagLen);
    llvm::errs() << "Sending diag len " << *(uint32_t*)offset << "\n";
    
    memcpy(offset, diags.data(), diagLen);
    offset += diagLen;
    memcpy(offset, objectCode.c_str(), objectCode.size());
    
    master.send(msg);
  }
  
  llvm::errs() << "compiler thread: exit\n";
  llvm::errs().flush();
      
  return NULL; // Suppress warning
}

// Steals work
void *DistccClientServer::SupplicationThread() {
  // Connect to peers
  std::vector<zmq::socket_t*> peers;
  MemoryBuffer *Buf = MemoryBuffer::getFile("/u/mike/config.txt");
  const char *start = Buf->getBufferStart();
  const char *end = Buf->getBufferEnd();
  char *startChar = (char*)start;
  char *curChar = (char *)start;
  
  // Get the hostname for the current machine
  char hostname[255];
  gethostname(hostname, 255);
  std::string myLoc("tcp://");
  myLoc += hostname;
  myLoc += ":5557";
  
  // Set up channel for receiving replies
  zmq::socket_t replyLine(zmqContext, ZMQ_P2P);
  replyLine.bind("tcp://127.0.0.1:5557");
  
  llvm::errs() << "connecting to peers\n";
  for ( ; curChar < end ; ++curChar) {
    //FIXME: Handle windows strings(\r\n)
    //FIXME: File must end with newline!
    //FIXME: Add support for comments(lines beginning with #)
    //FIXME: Remove whitespace, and ignore empty lines
    if(*curChar == '\n') {
      std::string addr(startChar, curChar - startChar);
      startChar = curChar+1;
      addr = "tcp://" + addr;
      addr += ":5558";
      
      // Don't create a socket for the current process
      // FIXME: support more than one process on a machine
      if (!strcmp(hostname, addr.c_str())) {
        continue;
      }
      
      zmq::socket_t *s = new zmq::socket_t(zmqContext, ZMQ_P2P);
      //NOTE: This is async connect, so it is fast, 
      //but the connection is not guaranteed to succeed!
      s->connect(addr.c_str());
      peers.push_back(s);
      
      llvm::errs() << "connected to " << addr << "\n";
    }
  }
  llvm::errs() << "connected to peers\n";
  
  // seed the prng
  srand((unsigned)time(0));

  while (1) {
    // block while there's work to do
    pthread_mutex_lock(&workQueueMutex);
    while (workQueue.size() > 0) {
      pthread_cond_wait(&noWork, &workQueueMutex);
    }
    pthread_mutex_unlock(&workQueueMutex);
    
    // choose a peer
    int peerNum = (rand()%peers.size());
    zmq::socket_t *peer = peers[peerNum];
    llvm::errs() << "requesting work from peer " << peerNum << "\n";
    
    // send request
    zmq::message_t request(myLoc.length() + 1);
    char *offset = (char *)request.data();
    memcpy(offset, myLoc.c_str(), myLoc.length()+1);
    peer->send(request);
    
    llvm::errs() << "awaiting response\n";
    
    // get reply
    zmq::message_t response;
    replyLine.recv(&response);  
    
    llvm::errs() << "received response\n";
    
    if (response.size() == 1) {
      continue; // peer's not interested
    }
    
    // add to work queue
    CompilerWork work = ProcessMessage(response);
    pthread_mutex_lock(&workQueueMutex);
    workQueue.push(work);
    pthread_cond_signal(&recievedWork);
    pthread_mutex_unlock(&workQueueMutex);
  }
  
  for (unsigned i = 0; i < peers.size(); i++) {
    delete peers[i];
  }
      
  return NULL; // Suppress warning
}

// lets other slaves steal work
void *DistccClientServer::DelegateThread() {
  zmq::socket_t supplicants(zmqContext, ZMQ_P2P);
  supplicants.bind("tcp://127.0.0.1:5558");
  
  while (1) {
    llvm::errs() << "waiting for a request\n";
    zmq::message_t msg;
    supplicants.recv(&msg);
    zmq::socket_t peer(zmqContext, ZMQ_P2P);
    peer.connect((char *)msg.data());
    llvm::errs() << "connected to " << (char *)msg.data() << "\n";
    
    DistccClientServer::CompilerWork work(0, "", "");
    pthread_mutex_lock(&workQueueMutex);
    if (workQueue.size() > 0) { // TODO: tune?
      work = workQueue.front();
      workQueue.pop();
    }
    pthread_mutex_unlock(&workQueueMutex);
    
    
    std::string response_str = ProcessCompilerWork(work);
    zmq::message_t response(response_str.length() + 1);
    char *offset = (char *) response.data();
    memcpy(offset, response_str.c_str(), response_str.length() + 1);
    peer.send(response);
    llvm::errs() << "sent work\n";
  }
  
  return NULL; // Suppress warning
}

std::string DistccClientServer::ProcessCompilerWork
(DistccClientServer::CompilerWork& work) {
  if (work.uniqueID == 0 &&
      work.args.size() == 0 &&
      work.source.size() == 0) {
      zmq::message_t msg(0);
      return "\0";
  }
  uint64_t uniqueID = work.uniqueID;
  uint32_t argLen = work.args.length();
  uint32_t sourceLen = work.source.length();
  
  int totalLen = sizeof(uniqueID) + sizeof(argLen) + argLen + sourceLen;
  char *buffer = new char[totalLen];
  int offset = 0;
  
  memcpy(&buffer[offset], &uniqueID, sizeof(uniqueID));
  offset += sizeof(uniqueID);
  
  memcpy(&buffer[offset], &argLen, sizeof(argLen));
  offset += sizeof(argLen);
  
  memcpy(&buffer[offset], work.args.c_str(), argLen);
  offset += argLen;
  
  memcpy(&buffer[offset], work.source.c_str(), sourceLen);
  offset += sourceLen;
  
  std::string str(buffer, totalLen);
  delete buffer;
  
  return str;
}

DistccClientServer::CompilerWork DistccClientServer::ProcessMessage
(zmq::message_t& msg) {
  uint64_t uniqueID;
  uint32_t argLen;
    
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
  
  CompilerWork work(uniqueID, args, source);
  return work;
}

// Static pthread helper method
void *DistccClientServer::pthread_RequestThread(void *ctx){
    return ((DistccClientServer *)ctx)->RequestThread();
}

void *DistccClientServer::pthread_CompilerThread(void *ctx){
    return ((DistccClientServer *)ctx)->CompilerThread();
}

void *DistccClientServer::pthread_SupplicationThread(void *ctx){
    return ((DistccClientServer *)ctx)->SupplicationThread();
}

void *DistccClientServer::pthread_DelegateThread(void *ctx){
    return ((DistccClientServer *)ctx)->DelegateThread();
}

// FIXME: Taken from Driver.cpp because the method isn't static...
// Presumably this will migrate to sys::Path eventually, so we can take this out
std::string DistccClientServer::GetTemporaryPath(const char *Suffix) const {
  // FIXME: This is lame; sys::Path should provide this function (in particular,
  // it should know how to find the temporary files dir).
  std::string Error;
  const char *TmpDir = ::getenv("TMPDIR");
  if (!TmpDir)
    TmpDir = ::getenv("TEMP");
  if (!TmpDir)
    TmpDir = ::getenv("TMP");
  if (!TmpDir)
    TmpDir = "/tmp";
  llvm::sys::Path P(TmpDir);
  P.appendComponent("cc");
  if (P.makeUnique(false, &Error)) {
    llvm::errs() << "Error making path!\n";
    return "";
  }
  
  // FIXME: Grumble, makeUnique sometimes leaves the file around!?  PR3837.
  P.eraseFromDisk(false, 0);
  
  P.appendSuffix(Suffix);
  return P.str();
}

