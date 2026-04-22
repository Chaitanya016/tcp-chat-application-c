#ifndef COMMON_H
#define COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define HOST_ADDR "127.0.0.1"
#define COMM_PORT 6333

#define MAX_NAME 30
#define MAX_PASS 30
#define MAX_MSG 100

typedef struct
{
    char uname[MAX_NAME];
    char passwd[MAX_PASS];
    int action;
} auth_info;

typedef struct
{
    char from[MAX_NAME];
    char to[MAX_NAME];
    char data[MAX_MSG];
    int mtype;
} msg_frame;

enum
{
    USR_LIST = 1,
    PRIVATE_CHAT,
    GROUP_CHAT,
    NOTIFY,
    EXIT_MSG,
    NOT_AVAIL,
    SERVER_CRASH
};

#endif