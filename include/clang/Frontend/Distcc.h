/*
 *  Distcc.h
 *  
 *
 *  Created by Michael Miller on 4/16/10.
 *  Copyright 2010 __MyCompanyName__. All rights reserved.
 *
 */
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/DistccClient.h"

#include "llvm/System/Mutex.h"

#include <sys/un.h>

using namespace llvm;

namespace clang {
class Distcc {
private:
	// Server-side methods/vars
	std::vector<DistccClient> localClients; //holds fds of local clients
	sys::Mutex localClientsMutex; //mutex protecting localClients vector
	
	pthread_t acceptThread;
	pthread_t preprocessThread;
	int acceptSocket;
	
	void *AcceptThread();
	void *PreprocessThread();
	
	//Boostrapping functions for pthreads
	static void *pthread_AcceptThread(void *ctx);
	static void *pthread_PreprocessThread(void *ctx);
	
	void startServer(struct sockaddr_un &addr);

	
	// Client-side methods/vars
	void startClient();
	int serverFd; // Fd with connection to the server
	CompilerInstance *CI;
	
	// Helper methods
	char *serializeArgVector(std::vector<std::string> &vec, int &length);
	std::vector<std::string> deserializeArgVector(char *string, int length);

	
public:
	Distcc(CompilerInstance &instance);
	~Distcc();

};

}
