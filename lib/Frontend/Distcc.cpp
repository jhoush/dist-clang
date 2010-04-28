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
#include "llvm/Support/MemoryBuffer.h"

// FIXME: Replace UNIX-specific operations with system-agnostc ones
#include <pthread.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <errno.h>
#include <pthread.h>
#include <limits.h>

// FIXME: Stop using zmq
#undef error_t
#include <zmq.hpp>

//FIXME: Handle sending return codes to Client!

using namespace clang;

//Note: http://beej.us/guide/bgipc/ was used as a reference when making socket code

Distcc::Distcc(CompilerInstance &instance)
: //FIXME: Determine proper value for app/io threads(respectively)
zmqContext(2, 2)
{

	counter = 0;
	currentSlave = 0;
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
			//Child(server)
			startServer(remote);	
		}
		else if(r>0){
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
			startClient();
			return;
		}
		else if(r<0){
			llvm::errs() << "Error forking\n";
			return;
		}
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
	
	
	ConnectToSlaves(); // This is OK to be nonthreaded b/c connections are created async by zmq

	// Start server to accept connections

	if(pthread_create(&acceptThread, NULL, pthread_AcceptThread, this)<0){
		llvm::errs() << "Error creating AcceptThread\n";
		close(acceptSocket);
	}
	
	if(pthread_create(&preprocessThread, NULL, pthread_PreprocessThread, this)<0){
		llvm::errs() << "Error creating preprocessor thread\n";
		close(acceptSocket);
	}
	
	if(pthread_create(&receiveThread, NULL, pthread_ReceiveThread, this)<0){
		llvm::errs() << "Error creating preprocessor thread\n";
		close(acceptSocket);
	}
	
	pthread_exit(NULL);
}

// This function's job is to preprocess files and send files to slaves

void *Distcc::PreprocessThread(){
	//FIXME: Timeout after some period
	FileManager fm; // Outside of loop so we have consistent cache
	while(1){
		
		clientsAwaitingDistributionMutex.acquire(); 
		//Pop file to send off queue
		DistccClient client = clientsAwaitingDistribution.front();
		clientsAwaitingDistribution.pop();
		clientsAwaitingDistributionMutex.release();

		//If no data sent, preprocess, and send out
		std::string preprocessedSource;
		llvm::raw_string_ostream *OS = new llvm::raw_string_ostream(preprocessedSource);
		
		
		TextDiagnosticBuffer DiagsBuffer;
		CompilerInvocation Invocation;
		Diagnostic Diags(&DiagsBuffer);
		
		
		std::vector<std::string> args = client.args;
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

		
		// FIXME: Note, this means we can't take stdin as input("-")
		const FileEntry *file = fm.getFile(args[args.size()-2].c_str()); 
		SourceMan.createMainFileID(file, SourceLocation());
		Preprocessor localPreprocessor(Diags, Invocation.getLangOpts(), *Target, SourceMan, hs);


		DoPrintPreprocessedInput(localPreprocessor, OS,
								 Invocation.getPreprocessorOutputOpts());
		OS->flush();
		delete OS;
		
		while(slaves.empty())
			usleep(50); // Wait until we have at least 1 slave to send to
		
		int argLen;
		char *serializedArgs = serializeArgVector(client.args, argLen);
		
		int preprocessedSourceLen = preprocessedSource.length() + 1; //For null byte
		
		int totalLen = preprocessedSourceLen + argLen + 4 + 8; /*
																length of preprocessed source + 
																argument length +
																length of integer which holds length of args +
																length of UID +
																*/
		
		// FIXME: Atomic increment instead of mutex?
		counterMutex.acquire();
		uint64_t uniqueID = counter++;
		counterMutex.release();
		
		// Compose message
		zmq::message_t msg(totalLen);
		char *offset = (char *)msg.data();
		memcpy(offset, &uniqueID, sizeof(uniqueID)); //Copy uniqueid into message 
		offset += sizeof(uniqueID);
		memcpy(offset, &argLen, sizeof(argLen)); //Copy arg length into message
		offset += sizeof(argLen);
		memcpy(offset, serializedArgs, argLen); //Copy args into message
		offset += argLen;
		memcpy(offset, preprocessedSource.c_str(), preprocessedSourceLen);
		offset += preprocessedSourceLen;
		
		slaves[currentSlave++]->send(msg);
		
		
		clientsAwaitingObjectCodeMutex.acquire();
		clientsAwaitingObjectCode.insert(std::pair<uint64_t,DistccClient>(uniqueID, client));
		clientsAwaitingObjectCodeMutex.release();

		
		free(serializedArgs);		
		if(clientsAwaitingDistribution.empty()) //Shouldn't need mutex here(worst case, we sleep)
			usleep(50); //Sleep if there's nothing to do for now
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

		
		free(argString);
		
		//Stick the client onto the end of the list
		clientsAwaitingDistributionMutex.acquire();
		clientsAwaitingDistribution.push(client);
		clientsAwaitingDistributionMutex.release();
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
	const char *relativeFileName = fe->getName();
	
	char path[PATH_MAX];
	if(path==NULL){
		llvm::errs() << "Error malloc'ing space for path\n";
		return;
	}
	if(realpath(relativeFileName, path)==NULL){
		llvm::errs() << "Error getcwd'ing\n";
		return;
	}
	std::string pathString(path);
	args.push_back(pathString); // Push working dir onto end of args
	
	llvm::errs() << "File is " << relativeFileName <<  "\n";
	
	//Send output filename to server
	args.push_back(CI->getFrontendOpts().OutputFile);
	
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
	
	
	int lengthRead = 0;
	int sizeOfDiags = 0;
	// Recieve length of diags
	if((lengthRead = recv(serverFd, &sizeOfDiags, sizeof(sizeOfDiags), MSG_WAITALL))<(int)sizeof(sizeOfDiags)){
		llvm::errs() << "Error recieving length of diags from server " << lengthRead << "\n";
		close(serverFd);
		return;
	}
	
	
	// Recieve diags themselves
	char *diags = (char *)malloc(sizeof(char)*sizeOfDiags);
	if((lengthRead = recv(serverFd, diags, sizeOfDiags, MSG_WAITALL))<(int)sizeOfDiags){
		llvm::errs() << "Error recieving diags from server " << lengthRead << "\n";
		close(serverFd);
		free(diags);
		return;
	}
	// FIXME: Do something other than print diags?
	llvm::errs() << diags;
	
	//FIXME: Use proper exit code(Make will continue even if file has errors!)
	
	free(diags);
}

void *Distcc::ConnectToSlaves(){
	//Setup context
	
	//FIXME: Remove hardcoding!
	llvm::errs() << "Constructing membuf\n";
	llvm::errs().flush();
	MemoryBuffer *Buf = MemoryBuffer::getFile("/Volumes/Data/Users/mike/Desktop/config.txt");
	llvm::errs() << "Finished constructing membuf\n";
	llvm::errs().flush();
	const char *start = Buf->getBufferStart();	
	const char *end = Buf->getBufferEnd();
	
	
	char *startChar = (char*)start;
	char *curChar = (char *)start;
	for ( ; curChar < end ; ++curChar){
		//FIXME: Handle windows strings(\r\n)
		//FIXME: File must end with newline!
		//FIXME: Add support for comments(lines beginning with #)
		//FIXME: Remove whitespace, and ignore empty lines
		if(*curChar == '\n'){
			std::string addr(startChar, curChar - startChar);
			startChar = curChar+1;
			
			zmq::socket_t *s = new zmq::socket_t(zmqContext,ZMQ_P2P);
			//NOTE: This is async connect, so it is fast, but the connection is not guaranteed to succeed!
			s->connect(addr.c_str());
			slaves.push_back(s);
		}
	}
	llvm::errs().flush();
}
//Recieve messages from slaves continuously, and write out object code as relevant
void *Distcc::ReceiveThread(){
	while(1){
		int slaveSize = slaves.size();
		for(int i=0;i<slaveSize;i++){
			zmq::message_t msg; //FIXME: Move this out of the loop to reuse message?
			if(slaves[i]->recv(&msg, ZMQ_NOBLOCK)<0){
				if(errno != EAGAIN){
					llvm::errs() << "Error reading from socket(not EAGAIN): " << errno << "\n";
				}
				continue; //No data available on slave, just try next one
			}
			//Process message
			int messageLen = msg.size();
			char *offset = (char *)msg.data();
			uint64_t uniqueID = *(uint64_t *)offset;
			offset += sizeof(uint64_t);
			uint32_t diagLen = *(uint32_t *)offset;
			offset += sizeof(uint32_t);
			char *diags  = (char*)offset;
			offset += diagLen;
			const char *objCode = (const char*)offset;
			int objLen = messageLen - (offset - (char*)msg.data());
			
			DistccClient client = clientsAwaitingObjectCode[uniqueID];
			
			//Write object code to disk
			std::string errorInfo;
			llvm::raw_fd_ostream outputStream(client.args[client.args.size()-1].c_str(), errorInfo);
			outputStream.write(objCode, objLen);
			outputStream.close();
			if(errorInfo.size() > 0)
				llvm::errs() << errorInfo << "\n";
			
			//Send length of diags to client, blocking
			if(send(client.fd, &diagLen, sizeof(diagLen), 0)<0){
				llvm::errs() << "Error sending diags back to client\n";
				//FIXME: Handle this error better
			}
			
			//Send diags to client, blocking
			//FIXME: Async?
			if(send(client.fd, diags, diagLen, 0)<0){
				llvm::errs() << "Error sending diags back to client\n";
				//FIXME: Handle this error better
			}
			
			//Close connection to client
			close(client.fd);
			
			// Take out client from 
			clientsAwaitingObjectCodeMutex.acquire();
			clientsAwaitingObjectCode.erase(clientsAwaitingObjectCode.find(uniqueID));
			clientsAwaitingObjectCodeMutex.release();
			
		}
		
	}
}
// Static pthread helper method
void *Distcc::pthread_AcceptThread(void *ctx){
	return ((Distcc *)ctx)->AcceptThread();
}
void *Distcc::pthread_PreprocessThread(void *ctx){
	return ((Distcc *)ctx)->PreprocessThread();
}
void *Distcc::pthread_ReceiveThread(void *ctx){
	return ((Distcc *)ctx)->ReceiveThread();
}

Distcc::~Distcc(){

}