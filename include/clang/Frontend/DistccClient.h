/*
 *  DistccClient.h
 *  PhotoBlur
 *
 *  Created by Michael Miller on 4/17/10.
 *  Copyright 2010 __MyCompanyName__. All rights reserved.
 *
 */

// DistccClient is a class to represent a makefile-spawned
// distcc process from the central server's perspective.

#ifndef LLVM_CLANG_DISTCCCLIENT_H
#include <vector>
#include <string>
#define LLVM_CLANG_DISTCCCLIENT_H
namespace clang {
	class DistccClient{
	public:
		int fd; // fd to talk to client
		std::vector<std::string> args;
		bool dataSent; //to slave
		bool dataReceived; //from slave
	};

}
#endif