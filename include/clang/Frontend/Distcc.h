/*
 *  Distcc.h
 *  
 *
 *  Created by Michael Miller on 4/16/10.
 *  Copyright 2010 __MyCompanyName__. All rights reserved.
 *
 */
#ifndef LLVM_CLANG_DISTCC_H
#define LLVM_CLANG_DISTCC_H
#include "clang/Frontend/CompilerInstance.h"

#include "llvm/System/Mutex.h"

#include <sys/un.h>

#include <queue>
#include <map>

// FIXME: Stop using zmq
#include <zmq.hpp>

using namespace llvm;

namespace clang {
  struct DistccClient{
    int fd; // fd to talk to client
    std::vector<std::string> args;
    std::string outputFile;
  };
	
class Distcc {
private:
  //Pipeline:
	
  //1. In AcceptThread, a new client is created and put onto
  //the clientsAwaitingDistribution queue
	
  //2. The preprocessThread pulls the request off the queue.
  //It then preprocesses the file, and assigns it a unique ID.
  //Then, it puts it into the clientsAwaitingObjectCode map
  //(which maps a unique ID to a client).
	
  //3. ReceiveThread is constantly looking for slaves to
  //receive data from. When it finds a message from the
  //slave with object code/diags, it recieves the object 
  //code and diags from the slave, writes the object code
  //to disk, and sends the diags back to the slave.
	
  //FIXME: Send preproc diags back as well as post-preproc diags!
	
	
  // Server-side methods/vars
  // Holds files which need to be distributed
  std::queue<DistccClient> clientsAwaitingDistribution;
  //mutex protecting filesAwaitingDistribution queue
  sys::Mutex clientsAwaitingDistributionMutex;
	
	
  // Mapping from unique identifier -> client
  // unique ID is used to quickly identify file when object code/diags come back
  // Only files whose source have been sent out to a slave are stored in this
  std::map<uint64_t, DistccClient> clientsAwaitingObjectCode;
  sys::Mutex clientsAwaitingObjectCodeMutex;
	
  // Used to ensure same unique idenifier doesn't get used twice
  uint64_t counter;
  // FIXME: Use atomic increment instead of mutex??
  sys::Mutex counterMutex;
	
  int currentSlave; // Used so we can round-robin slaves
	
  std::map<uint64_t,DistccClient> files;
	
	
  zmq::context_t zmqContext;
	
  pthread_t acceptThread;
  pthread_t preprocessThread;
  pthread_t receiveThread;
	
  int acceptSocket;
	
  void *AcceptThread();
  void *PreprocessThread();
  void *ReceiveThread();
	
  //Boostrapping functions for pthreads
  static void *pthread_AcceptThread(void *ctx);
  static void *pthread_PreprocessThread(void *ctx);
  static void *pthread_ReceiveThread(void *ctx);
	
  void startServer(struct sockaddr_un &addr);

	
  // Client-side methods/vars
  void startClient();
  int serverFd; // Fd with connection to the server
  CompilerInstance *CI;
	
  // Helper methods
  //char *serializeArgVector(std::vector<std::string> &vec, int &length);
  //std::vector<std::string> deserializeArgVector(char *string, int length);

	
public:
  Distcc(CompilerInstance &instance);	
  ~Distcc(){}
  static std::vector<std::string> deserializeArgVector(char *string,
                                                       int length);
  static char *serializeArgVector(std::vector<std::string> &vec, int &length);
};

	
}
#endif
