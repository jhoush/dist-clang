/*
 *  Distcc.cpp
 *  
 *
 *  Created by Michael Miller on 4/16/10.
 *  Copyright 2010 __MyCompanyName__. All rights reserved.
 *
 */

#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/Distcc.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Frontend/TextDiagnosticBuffer.h"
#include "clang/Frontend/Utils.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/FileManager.h"
#include "clang/Lex/HeaderSearch.h"
#include "clang/Lex/Preprocessor.h"

#include "llvm/Support/raw_ostream.h"

// FIXME: Replace UNIX-specific operations with system-agnostc ones
#include <pthread.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <errno.h>
#include <pthread.h>
#include <limits.h>
#include <stdio.h>


using namespace clang;

//Note: http://beej.us/guide/bgipc/ was used as a reference when making socket code

Distcc::Distcc(CompilerInstance &instance){

	
	this->CI = &instance;
	const char *socketPath = "/tmp/clangSocket";
	
	if((serverFd = socket(AF_UNIX, SOCK_STREAM, 0))==-1){
		llvm::errs() << "Error creating socket\n";
		return;
	}


	
	
	
	// Attempt to connect to server
	struct sockaddr_un remote;
	remote.sun_family = AF_UNIX;
	strcpy(remote.sun_path,socketPath);
	int r;
	if ((r=connect(serverFd, (struct sockaddr *)&remote, sizeof(remote))) == -1){
		//Didn't connect, so we have to fork!
		if((r=fork())==0){
			llvm::errs() << "Child\n";
			
			//Child(server)
			startServer(remote);	
			llvm::errs() << "Returned from server\n";

		}
		else if(r>0){
			llvm::errs() << "Parent\n";

			//Parent(client)
			// Sleep until (approx) when socket is ready,
			//
			// Sleep for 200 us( ~100us to create and bind to socket on my MBP,
			// using 200us as a ballpark w/ overhead and slow systems)
			// FIXME: Tune parameter more.
			usleep(200);
			
			//Spin until connected.
			//FIXME: Is 100 us the right amount of time to sleep?
			while (connect(serverFd, (struct sockaddr *)&remote, sizeof(remote)) == -1)
				usleep(100); 
			llvm::errs() << "ParentConnected\n";

			startClient();
			return;
		}
		else if(r<0){
			llvm::errs() << "Error forking\n";

		}
	}
	else {
		// TODO: Remove this
		llvm::errs() << "Ret code" << r <<  "\n";

	}
	
	//Connected to server, just start client
	startClient();
}


// Start the Distcc server
// (i.e. the central process accepting connections from Makefile-spawned processes)
void Distcc::startServer(struct sockaddr_un &addr){
	//Set up socket to recieve connections
	acceptSocket = socket(AF_UNIX, SOCK_STREAM, 0);
	if(bind(acceptSocket, (struct sockaddr *)&addr, sizeof(struct sockaddr_un))<0){
		// This means we hit the race condition
		// (another forked process spawned a server)
		llvm::errs() << "Error binding to socket(startServer)\n";
		exit(0);
	}
	// FIXME: Make second arg(# of queued commands) a parameter
	// The parameter should be equal to the -j flag in make, so
	// we never drop connections.
	if(listen(acceptSocket, 10)<0){
		llvm::errs() << "Error listening to socket(startServer)\n";

		close(acceptSocket);
	}
	
	// Start server to accept connections

	if(pthread_create(&acceptThread, NULL, pthread_AcceptThread, this)<0){
		llvm::errs() << "Error creating AcceptThread\n";

		close(acceptSocket);
	}
	
	if(pthread_create(&preprocessThread, NULL, pthread_PreprocessThread, this)<0){
		llvm::errs() << "Error creating preprocessor thread\n";

		close(acceptSocket);
	}

	pthread_exit(NULL);
}
void *Distcc::PreprocessThread(){
	//FIXME: Timeout after some period
	FileManager fm; // Outside of loop so we have consistent cache
	while(1){
		// Go through each client, check if we've sent out the args for the client yet
		std::vector<DistccClient>::iterator itr;
		localClientsMutex.acquire();
		for(itr = localClients.begin(); itr != localClients.end(); ++itr){
			if((*itr).dataSent == false){
				//If no data sent, preprocess, and send out
				std::string preprocessedSource;
				llvm::raw_string_ostream *OS = new llvm::raw_string_ostream(preprocessedSource);
				
				
				TextDiagnosticBuffer DiagsBuffer;
				CompilerInvocation Invocation;
				Diagnostic Diags(&DiagsBuffer);
				int length;
				
				
				std::vector<std::string> args = (*itr).args;
				llvm::SmallVector<const char *, 32> argAddresses;
				for(unsigned i=0;i<args.size();i++){
					argAddresses.push_back(args[i].c_str());
					llvm::errs() << args[i] << "\n";
				}
				
				CompilerInvocation::CreateFromArgs(Invocation, (const char **)argAddresses.begin(),
												   (const char **)argAddresses.end(), Diags);
				
				
				TargetInfo *Target = TargetInfo::CreateTargetInfo(Diags, Invocation.getTargetOpts());
				SourceManager SourceMan(Diags);
				HeaderSearch hs(fm);
				
				
				// FIXME: Is it bad to assume that first arg == filename
				// FIXME: Note, this means we can't take stdin as input("-")
				const FileEntry *file = fm.getFile(args[0].c_str()); 
				SourceMan.createMainFileID(file, SourceLocation());
				
				Preprocessor localPreprocessor(Diags, Invocation.getLangOpts(), *Target, SourceMan, hs);
				llvm::errs() << "Preprocessor created\n";


				DoPrintPreprocessedInput(localPreprocessor, OS,
										 Invocation.getPreprocessorOutputOpts());
				OS->flush();
				llvm::errs() << "Preprocessed source\n" << preprocessedSource << "\n";


				delete OS;
				const char *data = preprocessedSource.c_str();
				
				// FIXME: Actually send work out to nodes

				// Send back length of data
				length = preprocessedSource.length();
				if(send((*itr).fd, &length, sizeof(length), 0)<(int)sizeof(length)){
					llvm::errs() << "Error sending length of data to dumb makefile-spawned process\n";
				}
				
				//Send back actual data
				if(send((*itr).fd, data, length, 0)<(int)sizeof(length)){
					llvm::errs() << "Error sending actual data back to dumb makefile-spawned process\n";
				}
				
				(*itr).dataSent = true;
			}
		}
		localClientsMutex.release();
		usleep(50);
	}
}
//Args stored as <arg1>\0<arg2>\0<arg3>\0......<argN>\0
//Second param returns the length of the serialized string
char *Distcc::serializeArgVector(std::vector<std::string> &vec, int &length){
	//Get total size of string which needs to be serialized
	int serializedLength = 0;
	std::vector<std::string>::iterator itr;
	for(itr = vec.begin(); itr != vec.end(); ++itr){
		serializedLength += (*itr).length();
		serializedLength += 1; //For null byte
	}

	//Allocate memory for string to be serialized
	char *serializedString = (char *)malloc(sizeof(char) * serializedLength);
	
	//Re-iterate over array, copying string into mem
	for(itr = vec.begin(); itr != vec.end(); ++itr){
		int strLen = (*itr).length();
		memcpy(serializedString, (*itr).c_str(), strLen+1);
		serializedString += strLen;
		serializedString += 1; //For null byte
	}
	
	serializedString -= serializedLength; //Go back to beginning of string
	length = serializedLength;
	return serializedString; //Note: needs to be freed by caller!
}

std::vector<std::string> Distcc::deserializeArgVector(char *string, int length){
	// Thanks to Nicholas for the basis for this code
	// (which has been modified heavily since)
	std::vector<std::string> v;
	char *stringIter = string;
	while(stringIter-string < length) {
		int size = strlen(stringIter);
		v.push_back(std::string(stringIter));
		stringIter += size;
		stringIter += 1; //Gobble up null byte
	}
	return v;
}

//Thread will continuously accept new requests from local Makefile-spawned processes.
//The request fds will be placed in the "localClients" vector(locked by mutex).
void *Distcc::AcceptThread(){
	
	// Run Loop
	while(1){
		struct sockaddr_un remote;
		socklen_t len = sizeof(struct sockaddr_un);
		int fd2;
		llvm::errs() << "Beginning AcceptThread accept\n";

		if((fd2 = accept(acceptSocket, (struct sockaddr*)&remote, &len))==EAGAIN){
			//If nonblocking, this means, we need to retry
			llvm::errs() << "Retry\n";

			continue;
		}
		else if(fd2 < 0){
			//Other error
			llvm::errs() << "Error accepting socket\n";

			continue;
		}
		
		//Read the size of serialized arg vector
		uint32_t sizeOfString = 0; //In bytes
		int lengthRead = 0;
		if((lengthRead = recv(fd2, &sizeOfString, sizeof(sizeOfString), MSG_WAITALL))<(int)sizeof(sizeOfString)){
			llvm::errs() << "Error recieving data from socket(#1) " << lengthRead << "\n";

			close(fd2);
			continue;
		}
		
		llvm::errs() << "Finished AcceptThread accept\n";

		
		//Read the serialized string
		char *argString = (char *)malloc(sizeof(char)*sizeOfString);
		lengthRead = 0;
		if((lengthRead = recv(fd2, argString, sizeOfString, MSG_WAITALL))<(int)sizeOfString){
			llvm::errs() << "Error recieving data from socket(#2) " << lengthRead << "\n";

			close(fd2);
			continue;
		}
		
		
		DistccClient client;
		client.fd = fd2;
		client.args = deserializeArgVector(argString, sizeOfString);
		client.dataSent = false;
		client.dataReceived = false;
		
		free(argString);
		
		//Stick the client onto the end of the list
		localClientsMutex.acquire();
		localClients.push_back(client);
		llvm::errs() << "Pushed\n";

		localClientsMutex.release();
	}
	return NULL; //Should never hit
}

void Distcc::startClient(){
	// Get command line arguments
	std::vector<std::string> args;
	CI->getInvocation().toArgs(args);

	// FIXME: Remove more than -distribute arg
	int index = -1;
	for(unsigned i=0;i<args.size();i++){
		if(args[i] == "-distribute"){
			index = i;
			break;
		}
	}
	
	if(index!=-1)
		args.erase(args.begin()+index);
	
	// Add on name of file at end of args
	
	SourceManager &sm = CI->getSourceManager();
	const FileEntry *fe = sm.getFileEntryForID(sm.getMainFileID());
	std::string filename(fe->getName());
	args.push_back(filename); // Push filename onto end	of args
	
	char path[FILENAME_MAX];
	if(path==NULL){
		llvm::errs() << "Error malloc'ing space for path\n";
		return;
	}
	if(getcwd(path, sizeof(path))==NULL){
		llvm::errs() << "Error getcwd'ing\n";
		return;
	}
	std::string pathString(path);
	args.push_back(pathString); // Push working dir onto end of args
	
	llvm::errs() << "File is " << filename <<  "\n";
	
	
	// Send length of args
	int lengthOfArgs = 0;
	char *serializedArgs = serializeArgVector(args, lengthOfArgs);
	
	int lengthSent = 0;
	if((lengthSent = send(serverFd, &lengthOfArgs, sizeof(lengthOfArgs), 0))<(int)sizeof(lengthOfArgs)){
		llvm::errs() << "Error sending length of args\n";
		free(serializedArgs);
		return;
	}

	// Send args themselves
	if((lengthSent = send(serverFd, serializedArgs, lengthOfArgs, 0))<(int)lengthOfArgs){
		llvm::errs() << "Error sending arguments\n";
		free(serializedArgs);
		return;
	}
	free(serializedArgs);
	
	
	// FXIXME: Recieve diags
	llvm::errs() << "Recievining length\n";

	// FIXME: Recieve preprocessed source(remove after done)
	int sourceLength = 0;
	int lengthRead;
	if((lengthRead=recv(serverFd, &sourceLength, sizeof(sourceLength), MSG_WAITALL))<(int)sizeof(sourceLength)){
		llvm::errs() << "Error recieving length of source " << lengthRead << "\n" << "Errno: " << errno << "\n";
		exit(1); // TODO: Remove this
		return;
	}
	llvm::errs() << "Recieving source\n";

	char *source = (char *)malloc(sizeof(char)*sourceLength+1);
	source[sourceLength] = '\0'; // Tack on null byte
	if(recv(serverFd, source, sourceLength, MSG_WAITALL)<(int)sourceLength){
		llvm::errs() << "Error recieving source itself\n";
	}
	llvm::errs() << "Done recieving source\n";

	std::string sourceString(source);
	free(source);
	llvm::errs() << "Source string: " << sourceString << "\n";

	// FIXME: Recieve object code

}



// Static pthread helper method
void *Distcc::pthread_AcceptThread(void *ctx){
	return ((Distcc *)ctx)->AcceptThread();
}
void *Distcc::pthread_PreprocessThread(void *ctx){
	return ((Distcc *)ctx)->PreprocessThread();
}
Distcc::~Distcc(){

}
